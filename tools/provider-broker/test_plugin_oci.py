"""Tests for the OCI plugin (#285) through the broker, with a stand-in OCI SDK and client."""
from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provider_broker as pb  # noqa: E402
from plugin_oci import OciProvider, STANDARD_USD_PER_GB_MONTH, MONTH_S  # noqa: E402


class ServiceError(Exception):
    def __init__(self, status):
        super().__init__(f"status {status}")
        self.status = status


FAKE_OCI = SimpleNamespace(
    exceptions=SimpleNamespace(ServiceError=ServiceError),
    object_storage=SimpleNamespace(models=SimpleNamespace(CreateBucketDetails=lambda **kw: SimpleNamespace(**kw))))


class FakeClient:
    """The Object Storage calls the plugin makes, against an in-memory tenancy."""

    def __init__(self):
        self.buckets: dict[str, dict] = {}       # name -> {"details", "objects": {}, "uploads": []}
        self.calls: list[str] = []

    def _b(self, name):
        if name not in self.buckets:
            raise ServiceError(404)
        return self.buckets[name]

    def get_namespace(self):
        return SimpleNamespace(data="testns")

    def create_bucket(self, ns, details):
        assert ns == "testns" and details.public_access_type == "NoPublicAccess"
        if details.name in self.buckets:
            raise ServiceError(409)
        self.buckets[details.name] = {"details": details, "objects": {}, "uploads": []}

    def list_buckets(self, ns, compartment, fields=None, page=None, limit=None):
        assert fields == ["tags"]
        names = sorted(n for n, b in self.buckets.items() if b["details"].compartment_id == compartment)
        start = int(page or 0)
        chunk = names[start:start + 2]                      # tiny pages, to exercise paging
        data = [SimpleNamespace(name=n, freeform_tags=self.buckets[n]["details"].freeform_tags) for n in chunk]
        nxt = str(start + 2) if start + 2 < len(names) else None
        return SimpleNamespace(data=data, next_page=nxt)

    def get_bucket(self, ns, name, fields=None):
        b = self._b(name)
        return SimpleNamespace(data=SimpleNamespace(approximate_count=len(b["objects"]),
                                                    approximate_size=sum(map(len, b["objects"].values())),
                                                    public_access_type=b["details"].public_access_type))

    def list_objects(self, ns, name, start=None, limit=None):
        b = self._b(name)
        return SimpleNamespace(data=SimpleNamespace(objects=[SimpleNamespace(name=k) for k in b["objects"]],
                                                    next_start_with=None))

    def delete_object(self, ns, name, obj):
        del self._b(name)["objects"][obj]

    def list_multipart_uploads(self, ns, name):
        return SimpleNamespace(data=list(self._b(name)["uploads"]))

    def abort_multipart_upload(self, ns, name, obj, upload_id):
        b = self._b(name)
        b["uploads"] = [u for u in b["uploads"] if u.upload_id != upload_id]

    def delete_bucket(self, ns, name):
        b = self._b(name)
        if b["objects"] or b["uploads"]:
            raise ServiceError(409)                         # OCI refuses a non-empty bucket
        del self.buckets[name]


class NoCompute:
    """A tenancy where this identity has no compute permission (buckets only)."""

    def list_instances(self, comp):
        raise ServiceError(404)


def make(tmp_path, allow_spend=True):
    client = FakeClient()
    oci = OciProvider("unused", "ocid1.compartment.test", client=client, oci_module=FAKE_OCI,
                      compute=NoCompute(), network=object(), identity=object())
    b = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"oci": oci}, allow_spend=allow_spend)
    key = b.set_envelope(pb.Envelope(per_day_usd=1.0, per_action_usd=0.5, classes=["storage"],
                                     max_lease_s=7 * 86400))
    return b, client, key


def test_bucket_quote_is_worst_case_and_labelled_estimate(tmp_path):
    b, _, _ = make(tmp_path)
    q = b.quote("oci", "bucket", {"gb": 10}, 3 * 86400)
    assert q["max_cost_usd"] == pytest.approx(10 * STANDARD_USD_PER_GB_MONTH * 3 * 86400 / MONTH_S)
    assert q["exact"] is False and "Always Free" in q["free_tier"] and q["resource_class"] == "storage"
    for bad in ({"gb": 0}, {"gb": 5000}, {"gb": "lots"}, {"gb": True}):
        with pytest.raises(pb.Refused) as e:
            b.quote("oci", "bucket", bad, 3600)
        assert e.value.code == "bad_spec"
    with pytest.raises(pb.Refused) as e:
        b.quote("oci", "gpu", {}, 3600)
    assert e.value.code == "bad_kind"


def test_a_bucket_is_real_spending(tmp_path):
    b, client, _ = make(tmp_path, allow_spend=False)
    with pytest.raises(pb.Refused) as e:
        b.provision(b.quote("oci", "bucket", {"gb": 1}, 3600)["quote"])
    assert e.value.code == "spending_off" and client.buckets == {}


def test_provision_status_and_teardown_empties_then_deletes(tmp_path):
    b, client, _ = make(tmp_path)
    lease = b.provision(b.quote("oci", "bucket", {"gb": 1}, 86400)["quote"])
    name = lease["resource"]
    d = client.buckets[name]["details"]
    assert d.freeform_tags == {"qos-lease": f"qos-{b.broker_id}-{lease['lease']}"}
    assert d.storage_tier == "Standard" and d.versioning == "Disabled"
    client.buckets[name]["objects"] = {"a": b"12345", "b": b"xy"}
    client.buckets[name]["uploads"] = [SimpleNamespace(object="c", upload_id="u1")]
    st = b.status(lease["lease"])["provider_status"]
    assert st == {"state": "active", "objects": 2, "bytes": 7, "public": False}
    assert b.teardown(lease["lease"])["state"] == "ended"
    assert name not in client.buckets
    assert b.plugins["oci"].status(name) == {"state": "gone"}
    b.plugins["oci"].teardown(name)                          # already gone: idempotent


def test_find_returns_only_this_leases_bucket(tmp_path):
    b, client, _ = make(tmp_path)
    mine = [b.provision(b.quote("oci", "bucket", {}, 3600)["quote"]) for _ in range(3)]
    # someone else's buckets in the same compartment, tagged and untagged
    for n, tags in (("theirs", {"qos-lease": "qos-other-l0000000000"}), ("plain", {}),
                    ("lookalike", {"qos-lease": mine[0]["resource"] + "x"})):
        client.create_bucket("testns", FAKE_OCI.object_storage.models.CreateBucketDetails(
            name=n, compartment_id="ocid1.compartment.test", public_access_type="NoPublicAccess",
            storage_tier="Standard", versioning="Disabled", freeform_tags=tags))
    oci = b.plugins["oci"]
    for le in mine:
        assert oci.find(f"qos-{b.broker_id}-{le['lease']}") == [le["resource"]]
    for le in mine:
        b.teardown(le["lease"])
    assert sorted(client.buckets) == ["lookalike", "plain", "theirs"]


def test_setup_text_is_least_privilege(capsys):
    assert pb.main(["setup", "oci"]) == 0
    out = capsys.readouterr().out
    assert "manage buckets in compartment qos-broker" in out and "manage objects in compartment" in out
    assert "keep only \"API keys\"" in out and "never leaves the broker host" in out
    assert "tenancy" not in out.split("Policies")[1].split("5.")[0], "no tenancy-wide grant"
