"""OCI provider plugin (#285): Object Storage buckets and Always Free VMs, with the person's
own credentials.

Kinds:
  bucket   spec {"gb": <expected size, default 1>}. A private Standard-tier bucket in the
           configured compartment, tagged with the lease tag (freeform tag "qos-lease").
  vm       spec {"shape": "micro" | "a1", "ssh_public_key": "<ssh-ed25519 ... or ssh-rsa ...>",
                 "ocpus": 1-4 and "memory_gb": 1-24 (a1 only), "image": "oracle-linux" | "ubuntu",
                 "boot_gb": 50-200 (default 50)}.
           "micro" is VM.Standard.E2.1.Micro (x86, 1/8 OCPU, 1 GB); "a1" is VM.Standard.A1.Flex
           (Arm). Both are Always Free shapes. The VM gets a public IP on the broker's own
           network; the only way in is the SSH key the person gives.

Quotes are worst case and labelled estimates. A bucket is priced at the Standard storage list
price for its stated size. A VM's boot volume is priced as block storage (storage plus the
default performance units), and an A1 VM's OCPUs and memory at the A1 list price, in case the
tenancy's monthly Always Free allowance is used up. E2.1.Micro has no paid price: past the
Always Free limit OCI refuses the launch rather than billing it. Whether the free tier covers a
lease depends on what else the tenancy runs, so a quote never assumes it.

Network: VMs need a subnet. The first VM request creates one small network in the broker's
compartment (a VCN, an internet gateway, a default route to it and a public subnet, using the
VCN's default security list: SSH and ICMP in, everything out), tagged "qos-broker-net", and
every later VM reuses it. The network is not on any lease and is never torn down by the broker;
it costs nothing.

Teardown: a bucket is emptied (objects, unfinished multipart uploads), then deleted. A VM is
terminated with its boot volume. `find(tag)` returns only buckets and instances whose
"qos-lease" tag is exactly the lease's tag, so the broker never touches anything it did not
create.

The OCI identity this plugin uses needs, in its compartment only: manage buckets and objects
(buckets); manage instance-family, virtual-network-family and volume-family (VMs). See SETUP
below; the broker prints it with `provider_broker.py setup oci`.
"""
from __future__ import annotations

import re
import time

from provider_broker import Quote, Refused

STANDARD_USD_PER_GB_MONTH = 0.0255   # Object Storage Standard and block volume storage, list price
BLOCK_VPU_USD_PER_GB_MONTH = 0.017   # block volume performance, default 10 VPUs/GB at $0.0017
A1_USD_PER_OCPU_HOUR = 0.01
A1_USD_PER_GB_HOUR = 0.0015
MONTH_S = 30 * 86400
TAG_KEY = "qos-lease"
NET_TAG = "qos-broker-net"
MAX_GB = 1024
SHAPES = {"micro": "VM.Standard.E2.1.Micro", "a1": "VM.Standard.A1.Flex"}
IMAGES = {"oracle-linux": "Oracle Linux", "ubuntu": "Canonical Ubuntu"}
SSH_KEY = re.compile(r"^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp(256|384|521)) [A-Za-z0-9+/=]{40,}( [^\n]{0,200})?$")

SETUP = """\
OCI setup for the QuantumOS provider broker (run once, in the OCI console):
  1. Identity & Security > Compartments: create one for the broker, e.g. "qos-broker".
  2. Domains > Default > Users: create a user, e.g. "qos-broker"; Edit user capabilities:
     keep only "API keys".
  3. Groups: create "qos-broker" and add the user.
  4. Policies (root compartment), exactly (the first two for buckets, the rest for VMs):
       Allow group 'Default'/'qos-broker' to manage buckets in compartment qos-broker
       Allow group 'Default'/'qos-broker' to manage objects in compartment qos-broker
       Allow group 'Default'/'qos-broker' to manage instance-family in compartment qos-broker
       Allow group 'Default'/'qos-broker' to manage virtual-network-family in compartment qos-broker
       Allow group 'Default'/'qos-broker' to manage volume-family in compartment qos-broker
  5. The user > API keys > Paste a public key: paste the PEM the broker host generated
     (openssl genrsa -out key.pem 2048; openssl rsa -pubout -in key.pem). The private key
     never leaves the broker host.
  6. Put the configuration preview OCI shows into an OCI config file on the broker host,
     with key_file pointing at key.pem, and pass the compartment's OCID with --oci-compartment.
"""


class OciProvider:
    name = "oci"

    def __init__(self, config_file: str, compartment_id: str, profile: str = "DEFAULT", client=None,
                 oci_module=None, compute=None, network=None, identity=None, sleep=time.sleep):
        if oci_module is None:
            import oci as oci_module          # the SDK; tests pass a stand-in
        self.oci = oci_module
        self._cfg = None
        if client is None or compute is None or network is None or identity is None:
            self._cfg = oci_module.config.from_file(config_file, profile)
        self.c = client or oci_module.object_storage.ObjectStorageClient(self._cfg)
        self.compute = compute or oci_module.core.ComputeClient(self._cfg)
        self.net = network or oci_module.core.VirtualNetworkClient(self._cfg)
        self.ident = identity or oci_module.identity.IdentityClient(self._cfg)
        self.compartment = compartment_id
        self.ns = self.c.get_namespace().data
        self.sleep = sleep
        self._subnet: str | None = None

    def offerings(self) -> dict:
        return {"bucket": {"class": "storage", "unit": "GB-month", "price_usd": STANDARD_USD_PER_GB_MONTH,
                           "exact": False, "spec": {"gb": "expected size, 1 to 1024 (default 1)"},
                           "free_tier": "OCI Always Free: 20 GB in the home region, if the tenancy has room"},
                "vm": {"class": "compute", "unit": "hour", "exact": False,
                       "shapes": {"micro": "VM.Standard.E2.1.Micro (x86, 1/8 OCPU, 1 GB): Always Free only",
                                  "a1": f"VM.Standard.A1.Flex (Arm): ${A1_USD_PER_OCPU_HOUR}/OCPU-h + "
                                        f"${A1_USD_PER_GB_HOUR}/GB-h past the Always Free allowance"},
                       "spec": {"shape": "micro | a1", "ssh_public_key": "required",
                                "ocpus": "1-4 (a1)", "memory_gb": "1-24 (a1)",
                                "image": "oracle-linux | ubuntu", "boot_gb": "50-200 (default 50)"},
                       "free_tier": "OCI Always Free: 2 micro VMs; A1 3000 OCPU-h and 18000 GB-h a month; "
                                    "200 GB block volume in total"}}

    # ---- specs ----
    @staticmethod
    def _gb(spec: dict) -> float:
        gb = spec.get("gb", 1)
        if not isinstance(gb, (int, float)) or isinstance(gb, bool) or not 0 < gb <= MAX_GB:
            raise Refused("bad_spec", f"gb must be a number in (0, {MAX_GB}]")
        return float(gb)

    @staticmethod
    def _vm(spec: dict) -> dict:
        shape = spec.get("shape", "micro")
        if shape not in SHAPES:
            raise Refused("bad_spec", f"shape must be one of {', '.join(SHAPES)}")
        key = spec.get("ssh_public_key")
        if not isinstance(key, str) or not SSH_KEY.match(key.strip()):
            raise Refused("bad_spec", "ssh_public_key must be one OpenSSH public key line (ssh-ed25519 ..., ssh-rsa ...)")
        image = spec.get("image", "oracle-linux")
        if image not in IMAGES:
            raise Refused("bad_spec", f"image must be one of {', '.join(IMAGES)}")
        boot = spec.get("boot_gb", 50)
        if not isinstance(boot, int) or isinstance(boot, bool) or not 50 <= boot <= 200:
            raise Refused("bad_spec", "boot_gb must be an integer from 50 to 200")
        out = {"shape": shape, "ssh_public_key": key.strip(), "image": image, "boot_gb": boot}
        if shape == "a1":
            ocpus, mem = spec.get("ocpus", 1), spec.get("memory_gb", 6)
            for name, v, hi in (("ocpus", ocpus, 4), ("memory_gb", mem, 24)):
                if not isinstance(v, int) or isinstance(v, bool) or not 1 <= v <= hi:
                    raise Refused("bad_spec", f"{name} must be an integer from 1 to {hi}")
            out.update(ocpus=ocpus, memory_gb=mem)
        elif "ocpus" in spec or "memory_gb" in spec:
            raise Refused("bad_spec", "micro has a fixed size; ocpus and memory_gb are for a1")
        return out

    def quote(self, kind: str, spec: dict, duration_s: int) -> Quote:
        if kind == "bucket":
            gb = self._gb(spec)
            cost = gb * STANDARD_USD_PER_GB_MONTH * duration_s / MONTH_S
            return Quote("storage", "GB-month", STANDARD_USD_PER_GB_MONTH, round(cost, 6), exact=False,
                         free_tier="may be covered by OCI Always Free (20 GB, home region) if the tenancy has room",
                         notes=f"worst case at the Standard list price for {gb:g} GB; request charges excluded")
        if kind == "vm":
            v = self._vm(spec)
            hours = duration_s / 3600
            boot = v["boot_gb"] * (STANDARD_USD_PER_GB_MONTH + BLOCK_VPU_USD_PER_GB_MONTH) * duration_s / MONTH_S
            if v["shape"] == "a1":
                per_hour = v["ocpus"] * A1_USD_PER_OCPU_HOUR + v["memory_gb"] * A1_USD_PER_GB_HOUR
                compute = per_hour * hours
                note = (f"worst case: A1 at list price ({v['ocpus']} OCPU, {v['memory_gb']} GB) plus a "
                        f"{v['boot_gb']} GB boot volume, as if the Always Free allowance were used up")
            else:
                per_hour, compute = 0.0, 0.0
                note = (f"E2.1.Micro is Always Free only (past the limit OCI refuses the launch); "
                        f"worst case is the {v['boot_gb']} GB boot volume if the 200 GB free block storage is used up")
            return Quote("compute", "hour", round(per_hour, 6), round(compute + boot, 6), exact=False,
                         free_tier="may be covered entirely by OCI Always Free if the tenancy has room",
                         notes=note)
        raise Refused("bad_kind", f"oci offers: bucket, vm (asked for {kind!r})")

    # ---- provision ----
    def provision(self, kind: str, spec: dict, tag: str) -> str:
        if kind == "bucket":
            self._gb(spec)
            name = re.sub(r"[^A-Za-z0-9_.-]", "-", tag)[:256]
            details = self.oci.object_storage.models.CreateBucketDetails(
                name=name, compartment_id=self.compartment, public_access_type="NoPublicAccess",
                storage_tier="Standard", versioning="Disabled", freeform_tags={TAG_KEY: tag})
            self.c.create_bucket(self.ns, details)
            return name
        if kind == "vm":
            return self._launch(self._vm(spec), tag)
        raise Refused("bad_kind", f"oci offers: bucket, vm (asked for {kind!r})")

    def _wait(self, get, ok_state: str, timeout_s: float = 180) -> None:
        end = time.monotonic() + timeout_s
        while get().lifecycle_state != ok_state:
            if time.monotonic() > end:
                raise RuntimeError(f"timed out waiting for {ok_state}")
            self.sleep(2)

    def _ensure_network(self) -> str:
        """The broker's own public subnet, created on first use and found by its tag after."""
        if self._subnet:
            return self._subnet
        m = self.oci.core.models
        vcns = [v for v in self.net.list_vcns(self.compartment).data
                if (v.freeform_tags or {}).get(NET_TAG) and v.lifecycle_state == "AVAILABLE"]
        if vcns:
            vcn = vcns[0]
        else:
            vcn = self.net.create_vcn(m.CreateVcnDetails(
                compartment_id=self.compartment, cidr_blocks=["10.73.0.0/16"], display_name="qos-broker",
                dns_label="qosbroker", freeform_tags={NET_TAG: "1"})).data
            self._wait(lambda: self.net.get_vcn(vcn.id).data, "AVAILABLE")
            ig = self.net.create_internet_gateway(m.CreateInternetGatewayDetails(
                compartment_id=self.compartment, vcn_id=vcn.id, is_enabled=True, display_name="qos-broker",
                freeform_tags={NET_TAG: "1"})).data
            self._wait(lambda: self.net.get_internet_gateway(ig.id).data, "AVAILABLE")
            self.net.update_route_table(vcn.default_route_table_id, m.UpdateRouteTableDetails(route_rules=[
                m.RouteRule(destination="0.0.0.0/0", destination_type="CIDR_BLOCK", network_entity_id=ig.id)]))
        subnets = [s for s in self.net.list_subnets(self.compartment, vcn_id=vcn.id).data
                   if (s.freeform_tags or {}).get(NET_TAG) and s.lifecycle_state == "AVAILABLE"]
        if subnets:
            self._subnet = subnets[0].id
        else:
            sn = self.net.create_subnet(m.CreateSubnetDetails(
                compartment_id=self.compartment, vcn_id=vcn.id, cidr_block="10.73.0.0/24",
                display_name="qos-broker", dns_label="vms", prohibit_public_ip_on_vnic=False,
                freeform_tags={NET_TAG: "1"})).data
            self._wait(lambda: self.net.get_subnet(sn.id).data, "AVAILABLE")
            self._subnet = sn.id
        return self._subnet

    def _image(self, v: dict) -> str:
        imgs = self.compute.list_images(self.compartment, operating_system=IMAGES[v["image"]],
                                        shape=SHAPES[v["shape"]], sort_by="TIMECREATED",
                                        sort_order="DESC", lifecycle_state="AVAILABLE").data
        if not imgs:
            raise Refused("no_image", f"no {IMAGES[v['image']]} image for {SHAPES[v['shape']]} in this region")
        return imgs[0].id

    def _launch(self, v: dict, tag: str) -> str:
        m = self.oci.core.models
        subnet = self._ensure_network()
        image = self._image(v)
        ads = [a.name for a in self.ident.list_availability_domains(self.compartment).data]
        shape_cfg = (m.LaunchInstanceShapeConfigDetails(ocpus=float(v["ocpus"]), memory_in_gbs=float(v["memory_gb"]))
                     if v["shape"] == "a1" else None)
        last = None
        for ad in ads:      # Always Free capacity differs by availability domain; try each
            details = m.LaunchInstanceDetails(
                availability_domain=ad, compartment_id=self.compartment, display_name=tag,
                shape=SHAPES[v["shape"]], shape_config=shape_cfg, freeform_tags={TAG_KEY: tag},
                metadata={"ssh_authorized_keys": v["ssh_public_key"]},
                source_details=m.InstanceSourceViaImageDetails(image_id=image, boot_volume_size_in_gbs=v["boot_gb"]),
                create_vnic_details=m.CreateVnicDetails(subnet_id=subnet, assign_public_ip=True,
                                                        freeform_tags={TAG_KEY: tag}))
            try:
                return self.compute.launch_instance(details).data.id
            except self.oci.exceptions.ServiceError as e:
                msg = str(getattr(e, "message", "") or e)
                if e.status in (500, 400, 429) and ("capacity" in msg.lower() or "limit" in msg.lower()):
                    last = msg
                    continue
                raise
        raise Refused("no_capacity", f"no availability domain could launch {SHAPES[v['shape']]} now: {last}")

    # ---- status / teardown ----
    @staticmethod
    def _is_instance(rid: str) -> bool:
        return rid.startswith("ocid1.instance.")

    def status(self, rid: str) -> dict:
        if self._is_instance(rid):
            try:
                inst = self.compute.get_instance(rid).data
            except self.oci.exceptions.ServiceError as e:
                if e.status == 404:
                    return {"state": "gone"}
                raise
            out = {"state": inst.lifecycle_state, "shape": inst.shape, "availability_domain": inst.availability_domain}
            if inst.lifecycle_state == "RUNNING":
                for att in self.compute.list_vnic_attachments(self.compartment, instance_id=rid).data:
                    if att.lifecycle_state == "ATTACHED":
                        vnic = self.net.get_vnic(att.vnic_id).data
                        out.update(public_ip=vnic.public_ip, private_ip=vnic.private_ip)
                        break
            return out
        try:
            b = self.c.get_bucket(self.ns, rid, fields=["approximateCount", "approximateSize"]).data
        except self.oci.exceptions.ServiceError as e:
            if e.status == 404:
                return {"state": "gone"}
            raise
        return {"state": "active", "objects": b.approximate_count, "bytes": b.approximate_size,
                "public": b.public_access_type != "NoPublicAccess"}

    def teardown(self, rid: str) -> None:
        if self._is_instance(rid):
            try:
                if self.compute.get_instance(rid).data.lifecycle_state in ("TERMINATING", "TERMINATED"):
                    return
                self.compute.terminate_instance(rid, preserve_boot_volume=False)
            except self.oci.exceptions.ServiceError as e:
                if e.status != 404:
                    raise
            return
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
        return None   # OCI bills per tenancy; per-resource cost is not reported to us

    def find(self, tag: str) -> list[str]:
        out, page = [], None
        while True:
            r = self.c.list_buckets(self.ns, self.compartment, fields=["tags"], page=page, limit=1000)
            out += [b.name for b in r.data if (b.freeform_tags or {}).get(TAG_KEY) == tag]
            page = r.next_page
            if not page:
                break
        try:
            insts = self.compute.list_instances(self.compartment).data
        except self.oci.exceptions.ServiceError as e:
            if e.status in (401, 404):    # no compute permission: the broker cannot have made any
                return out
            raise
        out += [i.id for i in insts if (i.freeform_tags or {}).get(TAG_KEY) == tag
                and i.lifecycle_state not in ("TERMINATING", "TERMINATED")]
        return out
