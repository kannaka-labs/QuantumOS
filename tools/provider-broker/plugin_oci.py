"""OCI provider plugin (#285): Object Storage buckets, with the person's own credentials.

Kinds:
  bucket   spec {"gb": <expected size, default 1>}. A private Standard-tier bucket in the
           configured compartment, tagged with the lease tag (freeform tag "qos-lease").

Quotes are worst case: the Standard storage list price for the stated size over the whole
lease, flagged as an estimate. OCI's Always Free tier (20 GB of Object Storage in the home
region) may make it cost nothing, but whether it does depends on what else the tenancy
stores, so the quote never assumes it. Request charges are not included (the first 50,000
requests a month are free).

Teardown empties the bucket (objects and unfinished multipart uploads), then deletes it.
`find(tag)` lists the compartment's buckets and returns only those whose "qos-lease" tag is
exactly the lease's tag, so the broker never touches a bucket it did not create.

The OCI identity this plugin uses needs, in its compartment: manage buckets, manage objects
(see SETUP below; the broker prints it with `provider_broker.py setup oci`).
"""
from __future__ import annotations

import re

from provider_broker import Quote, Refused

STANDARD_USD_PER_GB_MONTH = 0.0255   # OCI price list, Object Storage Standard; an estimate
MONTH_S = 30 * 86400
TAG_KEY = "qos-lease"
MAX_GB = 1024

SETUP = """\
OCI setup for the QuantumOS provider broker (run once, in the OCI console):
  1. Identity & Security > Compartments: create one for the broker, e.g. "qos-broker".
  2. Domains > Default > Users: create a user, e.g. "qos-broker"; Edit user capabilities:
     keep only "API keys".
  3. Groups: create "qos-broker" and add the user.
  4. Policies (root compartment), exactly:
       Allow group 'Default'/'qos-broker' to manage buckets in compartment qos-broker
       Allow group 'Default'/'qos-broker' to manage objects in compartment qos-broker
  5. The user > API keys > Paste a public key: paste the PEM the broker host generated
     (openssl genrsa -out key.pem 2048; openssl rsa -pubout -in key.pem). The private key
     never leaves the broker host.
  6. Put the configuration preview OCI shows into an OCI config file on the broker host,
     with key_file pointing at key.pem, and pass the compartment's OCID with --oci-compartment.
"""


class OciProvider:
    name = "oci"

    def __init__(self, config_file: str, compartment_id: str, profile: str = "DEFAULT", client=None,
                 oci_module=None):
        if oci_module is None:
            import oci as oci_module          # the SDK; tests pass a stand-in
        self.oci = oci_module
        if client is None:
            cfg = oci_module.config.from_file(config_file, profile)
            client = oci_module.object_storage.ObjectStorageClient(cfg)
        self.c = client
        self.compartment = compartment_id
        self.ns = self.c.get_namespace().data

    def offerings(self) -> dict:
        return {"bucket": {"class": "storage", "unit": "GB-month", "price_usd": STANDARD_USD_PER_GB_MONTH,
                           "exact": False, "spec": {"gb": "expected size, 1 to 1024 (default 1)"},
                           "free_tier": "OCI Always Free: 20 GB in the home region, if the tenancy has room"}}

    @staticmethod
    def _gb(spec: dict) -> float:
        gb = spec.get("gb", 1)
        if not isinstance(gb, (int, float)) or isinstance(gb, bool) or not 0 < gb <= MAX_GB:
            raise Refused("bad_spec", f"gb must be a number in (0, {MAX_GB}]")
        return float(gb)

    def quote(self, kind: str, spec: dict, duration_s: int) -> Quote:
        if kind != "bucket":
            raise Refused("bad_kind", f"oci offers: bucket (asked for {kind!r})")
        gb = self._gb(spec)
        cost = gb * STANDARD_USD_PER_GB_MONTH * duration_s / MONTH_S
        return Quote("storage", "GB-month", STANDARD_USD_PER_GB_MONTH, round(cost, 6), exact=False,
                     free_tier="may be covered by OCI Always Free (20 GB, home region) if the tenancy has room",
                     notes=f"worst case at the Standard list price for {gb:g} GB; request charges excluded")

    def provision(self, kind: str, spec: dict, tag: str) -> str:
        if kind != "bucket":
            raise Refused("bad_kind", f"oci offers: bucket (asked for {kind!r})")
        self._gb(spec)
        name = re.sub(r"[^A-Za-z0-9_.-]", "-", tag)[:256]
        details = self.oci.object_storage.models.CreateBucketDetails(
            name=name, compartment_id=self.compartment, public_access_type="NoPublicAccess",
            storage_tier="Standard", versioning="Disabled", freeform_tags={TAG_KEY: tag})
        self.c.create_bucket(self.ns, details)
        return name

    def status(self, rid: str) -> dict:
        try:
            b = self.c.get_bucket(self.ns, rid, fields=["approximateCount", "approximateSize"]).data
        except self.oci.exceptions.ServiceError as e:
            if e.status == 404:
                return {"state": "gone"}
            raise
        return {"state": "active", "objects": b.approximate_count, "bytes": b.approximate_size,
                "public": b.public_access_type != "NoPublicAccess"}

    def teardown(self, rid: str) -> None:
        try:
            start = None
            while True:
                r = self.c.list_objects(self.ns, rid, start=start, limit=1000).data
                for o in r.objects:
                    self.c.delete_object(self.ns, rid, o.name)
                if not r.next_start_with:
                    break
                start = r.next_start_with
            for u in self.c.list_multipart_uploads(self.ns, rid).data:
                self.c.abort_multipart_upload(self.ns, rid, u.object, u.upload_id)
            self.c.delete_bucket(self.ns, rid)
        except self.oci.exceptions.ServiceError as e:
            if e.status != 404:      # already gone: teardown is idempotent
                raise

    def billed(self, rid: str) -> float | None:
        return None   # OCI bills per tenancy; per-bucket cost is not reported to us

    def find(self, tag: str) -> list[str]:
        out, page = [], None
        while True:
            r = self.c.list_buckets(self.ns, self.compartment, fields=["tags"], page=page, limit=1000)
            out += [b.name for b in r.data if (b.freeform_tags or {}).get(TAG_KEY) == tag]
            page = r.next_page
            if not page:
                return out
