"""Tests for the OCI plugin's VMs (#285 part 2), with stand-in compute, network and identity
clients. The bucket tests are in test_plugin_oci.py."""
from __future__ import annotations

import itertools
import sys
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provider_broker as pb  # noqa: E402
from plugin_oci import OciProvider, BLOCK_VPU_USD_PER_GB_MONTH, MONTH_S, STANDARD_USD_PER_GB_MONTH  # noqa: E402
from test_plugin_oci import FAKE_OCI as BASE, FakeClient, ServiceError  # noqa: E402

KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGZrZWtleWZvcnRoZXRlc3RzaW5xdWFudHVtb3M nick@test"
ids = itertools.count(1)


def model(**kw):
    return NS(**kw)


FAKE_OCI = NS(exceptions=BASE.exceptions, object_storage=BASE.object_storage,
              core=NS(models=NS(**{n: model for n in (
                  "CreateVcnDetails", "CreateInternetGatewayDetails", "UpdateRouteTableDetails", "RouteRule",
                  "CreateSubnetDetails", "LaunchInstanceShapeConfigDetails", "LaunchInstanceDetails",
                  "InstanceSourceViaImageDetails", "CreateVnicDetails")})))


class FakeNet:
    def __init__(self):
        self.vcns, self.igs, self.subnets, self.routes, self.vnics = {}, {}, {}, {}, {}
        self.created = []

    def list_vcns(self, comp):
        return NS(data=list(self.vcns.values()))

    def create_vcn(self, d):
        v = NS(id=f"ocid1.vcn.{next(ids)}", freeform_tags=d.freeform_tags, lifecycle_state="AVAILABLE",
               default_route_table_id=f"ocid1.rt.{next(ids)}", cidr_blocks=d.cidr_blocks)
        self.vcns[v.id] = v
        self.created.append("vcn")
        return NS(data=v)

    def get_vcn(self, i):
        return NS(data=self.vcns[i])

    def create_internet_gateway(self, d):
        g = NS(id=f"ocid1.ig.{next(ids)}", lifecycle_state="AVAILABLE")
        self.igs[g.id] = g
        self.created.append("ig")
        return NS(data=g)

    def get_internet_gateway(self, i):
        return NS(data=self.igs[i])

    def update_route_table(self, rt, d):
        self.routes[rt] = d.route_rules
        self.created.append("route")

    def list_subnets(self, comp, vcn_id=None):
        return NS(data=[s for s in self.subnets.values() if s.vcn_id == vcn_id])

    def create_subnet(self, d):
        s = NS(id=f"ocid1.subnet.{next(ids)}", vcn_id=d.vcn_id, freeform_tags=d.freeform_tags,
               lifecycle_state="AVAILABLE", prohibit_public_ip_on_vnic=d.prohibit_public_ip_on_vnic)
        self.subnets[s.id] = s
        self.created.append("subnet")
        return NS(data=s)

    def get_subnet(self, i):
        return NS(data=self.subnets[i])

    def get_vnic(self, i):
        return NS(data=self.vnics[i])


class FakeCompute:
    def __init__(self, net, full_ads=()):
        self.net, self.full_ads = net, set(full_ads)
        self.instances: dict[str, NS] = {}
        self.launches: list[NS] = []

    def list_images(self, comp, operating_system=None, shape=None, **kw):
        return NS(data=[NS(id=f"img-{operating_system}-{shape}")])

    def launch_instance(self, d):
        self.launches.append(d)
        if d.availability_domain in self.full_ads:
            e = ServiceError(500)
            e.message = "Out of host capacity."
            raise e
        i = NS(id=f"ocid1.instance.{next(ids)}", lifecycle_state="RUNNING", shape=d.shape,
               availability_domain=d.availability_domain, freeform_tags=d.freeform_tags)
        self.instances[i.id] = i
        vnic = f"ocid1.vnic.{next(ids)}"
        self.net.vnics[vnic] = NS(public_ip="203.0.113.7", private_ip="10.73.0.5")
        i.vnic = vnic
        return NS(data=i)

    def get_instance(self, i):
        if i not in self.instances:
            raise ServiceError(404)
        return NS(data=self.instances[i])

    def list_vnic_attachments(self, comp, instance_id=None):
        return NS(data=[NS(lifecycle_state="ATTACHED", vnic_id=self.instances[instance_id].vnic)])

    def terminate_instance(self, i, preserve_boot_volume=True):
        assert preserve_boot_volume is False, "the boot volume must go with the VM"
        self.instances[i].lifecycle_state = "TERMINATED"

    def list_instances(self, comp):
        return NS(data=list(self.instances.values()))


class FakeIdentity:
    def list_availability_domains(self, comp):
        return NS(data=[NS(name="AD-1"), NS(name="AD-2"), NS(name="AD-3")])


def make(tmp_path, full_ads=()):
    tmp_path.mkdir(parents=True, exist_ok=True)
    net = FakeNet()
    comp = FakeCompute(net, full_ads)
    oci = OciProvider("unused", "ocid1.compartment.test", client=FakeClient(), oci_module=FAKE_OCI,
                      compute=comp, network=net, identity=FakeIdentity(), sleep=lambda s: None)
    b = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"oci": oci}, allow_spend=True)
    b.set_envelope(pb.Envelope(per_day_usd=1.0, per_action_usd=0.5, classes=["compute", "storage"],
                               max_lease_s=7 * 86400))
    return b, oci, comp, net


def test_quotes_are_worst_case(tmp_path):
    b, *_ = make(tmp_path)
    day = 86400
    q = b.quote("oci", "vm", {"shape": "micro", "ssh_public_key": KEY}, day)
    boot = 50 * (STANDARD_USD_PER_GB_MONTH + BLOCK_VPU_USD_PER_GB_MONTH) * day / MONTH_S
    assert q["max_cost_usd"] == pytest.approx(boot, abs=1e-6) and q["exact"] is False
    assert q["unit_price_usd"] == 0 and "Always Free only" in q["notes"]
    q = b.quote("oci", "vm", {"shape": "a1", "ocpus": 2, "memory_gb": 12, "ssh_public_key": KEY}, day)
    assert q["max_cost_usd"] == pytest.approx((2 * 0.01 + 12 * 0.0015) * 24 + boot, abs=1e-6)
    assert "as if the Always Free allowance were used up" in q["notes"]


@pytest.mark.parametrize("spec", [
    {"shape": "micro"},                                            # no key
    {"shape": "micro", "ssh_public_key": "not a key"},
    {"shape": "micro", "ssh_public_key": KEY + "\nssh-rsa AAAA second-line"},
    {"shape": "big", "ssh_public_key": KEY},
    {"shape": "micro", "ssh_public_key": KEY, "ocpus": 2},          # micro is fixed
    {"shape": "a1", "ssh_public_key": KEY, "ocpus": 5},
    {"shape": "a1", "ssh_public_key": KEY, "memory_gb": 0},
    {"shape": "micro", "ssh_public_key": KEY, "boot_gb": 20},
    {"shape": "micro", "ssh_public_key": KEY, "image": "windows"},
])
def test_bad_vm_specs(tmp_path, spec):
    b, *_ = make(tmp_path)
    with pytest.raises(pb.Refused) as e:
        b.quote("oci", "vm", spec, 3600)
    assert e.value.code == "bad_spec"


def test_vm_lifecycle_builds_the_network_once(tmp_path):
    b, oci, comp, net = make(tmp_path)
    l1 = b.provision(b.quote("oci", "vm", {"shape": "micro", "ssh_public_key": KEY}, 3600)["quote"])
    assert net.created == ["vcn", "ig", "route", "subnet"]
    (rt,) = net.routes.values()
    assert rt[0].destination == "0.0.0.0/0"
    d = comp.launches[-1]
    assert d.shape == "VM.Standard.E2.1.Micro" and d.metadata == {"ssh_authorized_keys": KEY}
    assert d.freeform_tags == {"qos-lease": f"qos-{b.broker_id}-{l1['lease']}"}
    assert d.create_vnic_details.assign_public_ip is True and d.source_details.boot_volume_size_in_gbs == 50
    st = b.status(l1["lease"])["provider_status"]
    assert st["state"] == "RUNNING" and st["public_ip"] == "203.0.113.7"
    l2 = b.provision(b.quote("oci", "vm", {"shape": "a1", "ocpus": 1, "memory_gb": 6, "ssh_public_key": KEY,
                                           "image": "ubuntu"}, 3600)["quote"])
    assert net.created == ["vcn", "ig", "route", "subnet"], "the second VM reuses the network"
    assert comp.launches[-1].shape_config.ocpus == 1.0 and "Canonical Ubuntu" in comp.launches[-1].source_details.image_id
    # a fresh provider (a broker restart) finds the tagged network instead of building another
    oci2 = OciProvider("unused", "ocid1.compartment.test", client=FakeClient(), oci_module=FAKE_OCI,
                       compute=comp, network=net, identity=FakeIdentity(), sleep=lambda s: None)
    assert oci2._ensure_network() == oci._subnet and net.created.count("vcn") == 1
    for le in (l1, l2):
        assert b.teardown(le["lease"])["state"] == "ended"
    assert all(i.lifecycle_state == "TERMINATED" for i in comp.instances.values())
    oci.teardown(l1["resource"])                                     # idempotent
    assert b.status(l1["lease"])["state"] == "ended"


def test_full_availability_domains_are_skipped(tmp_path):
    b, _, comp, _ = make(tmp_path, full_ads=("AD-1", "AD-2"))
    le = b.provision(b.quote("oci", "vm", {"shape": "a1", "ssh_public_key": KEY}, 3600)["quote"])
    assert [d.availability_domain for d in comp.launches] == ["AD-1", "AD-2", "AD-3"]
    assert b.status(le["lease"])["provider_status"]["availability_domain"] == "AD-3"
    b2, _, comp2, _ = make(tmp_path / "x", full_ads=("AD-1", "AD-2", "AD-3"))
    with pytest.raises(pb.Refused) as e:
        b2.provision(b2.quote("oci", "vm", {"shape": "a1", "ssh_public_key": KEY}, 3600)["quote"])
    assert e.value.code == "provision_failed" and "capacity" in str(e.value)
    assert comp2.instances == {}


def test_find_returns_only_this_leases_vm(tmp_path):
    b, oci, comp, _ = make(tmp_path)
    le = b.provision(b.quote("oci", "vm", {"shape": "micro", "ssh_public_key": KEY}, 3600)["quote"])
    comp.instances["ocid1.instance.theirs"] = NS(id="ocid1.instance.theirs", lifecycle_state="RUNNING",
                                                 freeform_tags={"qos-lease": "qos-other-l0000000000"})
    comp.instances["ocid1.instance.plain"] = NS(id="ocid1.instance.plain", lifecycle_state="RUNNING",
                                                freeform_tags={})
    tag = f"qos-{b.broker_id}-{le['lease']}"
    assert oci.find(tag) == [le["resource"]]
    b.teardown(le["lease"])
    assert oci.find(tag) == [], "a terminated VM is not returned again"
    assert comp.instances["ocid1.instance.theirs"].lifecycle_state == "RUNNING"
    assert comp.instances["ocid1.instance.plain"].lifecycle_state == "RUNNING"


def test_setup_lists_the_vm_policies():
    from plugin_oci import SETUP
    for fam in ("instance-family", "virtual-network-family", "volume-family"):
        assert f"manage {fam} in compartment qos-broker" in SETUP
    assert " in tenancy" not in SETUP
