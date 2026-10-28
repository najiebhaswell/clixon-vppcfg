# clixon-vppcfg

A Cisco-style and Huawei VRP-style CLI (and NETCONF) for [VPP](https://fd.io/) interface configuration, built with
[Clixon](https://github.com/clicon/clixon) **on top of [vppcfg](https://git.ipng.ch/ipng/vppcfg)**.

Clixon provides the candidate/running datastores, validate, commit, confirmed commit and the CLI.
vppcfg remains the engine that validates the configuration against the dataplane and applies it to VPP.

```
 rtr (Cisco style)   vrp (Huawei VRP style)   NETCONF
          └──────────────────┬───────────────────┘
              clixon_backend 7.9 (candidate / running datastores)
                          │ backend_vppcfg.so (validate / commit / revert)
                          ▼
              clixon-vppcfg (Python helper)
                ├─ validate: XML -> temporary YAML -> vppcfg check + plan
                └─ apply:    /etc/vpp/vppcfg.yaml -> vppcfg apply -> /etc/vpp/config/vppcfg.vpp (boot)
                          ▼
                         VPP
```

- The YANG model (`yang/vppcfg@2026-10-28.yang`) maps 1:1 to the vppcfg 1.2.x schema.
  Phase 1 covers physical interfaces, sub-interfaces, loopbacks and BondEthernet.
- `/etc/vpp/vppcfg.yaml` is **generated** on every commit; do not edit it by hand.
- At VPP start the configuration is still loaded from `/etc/vpp/config/vppcfg.vpp`, so the dataplane
  comes up even if the Clixon backend is not running.
- Every committed YAML is kept in `/var/lib/clixon-vppcfg/history/`.

Tested on Debian 13, VPP 26.06 (DPDK, linux-cp), vppcfg 1.2.4, Clixon/CLIgen 7.9.0 and BIRD 3.1.

## Requirements

- VPP with the `linux_cp` and `linux_nl` plugins, vppcfg 1.2.4 at `/usr/local/sbin/vppcfg`, `python3-yaml`.
- CLIgen and Clixon **7.9.0** installed in `/usr/local`:

  ```bash
  sudo apt-get install -y git flex bison libssl-dev libnghttp2-dev pkg-config python3-yaml
  sudo groupadd --system clicon
  git clone --branch 7.9.0 --depth 1 https://github.com/clicon/cligen.git
  git clone --branch 7.9.0 --depth 1 https://github.com/clicon/clixon.git
  (cd cligen && ./configure && make && sudo make install) && sudo ldconfig
  (cd clixon && ./configure --with-restconf=native && make && sudo make install && sudo make install-include) && sudo ldconfig
  ```

## Installation

```bash
make && sudo make install
sudo install -d -m 0750 -g clicon /var/lib/clixon-vppcfg
# One-time migration: import the existing vppcfg.yaml as the running datastore
/usr/local/libexec/clixon-vppcfg/clixon-vppcfg import /etc/vpp/vppcfg.yaml \
  | sudo tee /var/lib/clixon-vppcfg/running_db >/dev/null
# Operators need the clicon (backend socket), vpp (vppctl) and bird (birdc) groups
sudo usermod -aG clicon,vpp,bird <user>
sudo systemctl daemon-reload && sudo systemctl enable --now clixon-vppcfg
```

In the VPP startup config use `lcp lcp-auto-subint off`: sub-interface LCPs are managed explicitly
(`lcp cx5-0.100`). With `on`, vppcfg deletes the automatically created TAPs on the next commit.

## Usage

```
$ rtr
BR# configure
BR(config)# interface cx5-0.100
BR(config-if)# encapsulation dot1q 100 exact-match
BR(config-if)# ip address 198.51.100.1/30
BR(config-if)# lcp cx5-0.100
BR(config-if)# end
% There are uncommitted changes (see 'show configuration diff').
% Use 'commit' to apply them or 'abort' to discard them.
BR(config-if)# show configuration diff
BR(config-if)# commit                   (or: commit confirmed 5)
% Commit complete.
```

`TAB` completes the command; a second `TAB` (or `?`) lists each option with its help text. Commands can also be run
one-shot: `rtr show running-config`.

| Mode | Commands |
|---|---|
| exec (`BR#`) | `configure [terminal]`, `show ...`, `exit` |
| configure (`BR(config)#`) | `interface <name>`, `no interface <name>`, `commit`, `validate`, `abort`, `end` |
| all interface modes | `description`, `mtu`, `ip address A.B.C.D/n`, `ipv6 address X::/n`, `lcp`, `unnumbered`, `mpls enable`, `no ...`, `exit`, plus `commit`/`validate`/`abort`/`end`/`show` |
| physical (`BR(config-if)#`) | `mac-address`, `shutdown`, `l2xc`, `sflow enable` |
| sub-interface (`BR(config-subif)#`) | `encapsulation dot1q\|dot1ad <vid> [inner-dot1q <vid>] [exact-match]`, `shutdown`, `l2xc` |
| loopback (`BR(config-if)#`) | `mac-address` |
| bond (`BR(config-if)#`) | `member`, `mode`, `load-balance`, `mac-address`, `shutdown`, `l2xc` |
| show | `running-config`, `candidate-config`, `configuration diff`, `interfaces [name]`, `hardware [name]`, `ip interface brief`, `ip route summary\|<prefix>`, `ipv6 route ...`, `lcp`, `bond`, `bgp summary\|neighbor <name>`, `version` |

The interface name selects its type and mode: `loopN` (loopback), `BondEthernetN` (bond), `parent.N`
(sub-interface), anything else is a physical interface. Each type has its own mode, so `?` only lists
the commands that apply, and `TAB` on `interface` shows each interface's type and description. A physical interface that does not exist in
VPP is rejected as soon as you type `interface <name>`; loopbacks and bonds may be new, and `TAB`
suggests the next free `loopN` and `BondEthernetN`.

### Huawei VRP style (`vrp`)

`vrp` is a second CLI on the same datastore and commit path, in Huawei VRP8 two-stage style. Both CLIs
can be used side by side; a change committed in one is visible in the other.

```
$ vrp
<BR> display current-configuration
<BR> system-view
[~BR] interface Eth-Trunk0
[~BR-Eth-Trunk0] mode lacp-static
[~BR-Eth-Trunk0] load-balance src-dst-ip-port
[*BR-Eth-Trunk0] trunkport cx5-1
[*BR-Eth-Trunk0] ip address 198.51.100.1 30
[*BR-Eth-Trunk0] display configuration candidate changes
[*BR-Eth-Trunk0] commit                  (or: commit trial 300)
[~BR-Eth-Trunk0] quit
[~BR] return
<BR> display ip interface brief
```

- Prompt: `<BR>` user view, `[~BR]` system view, `[~BR-<interface>]` interface view; `*` instead of `~`
  when the candidate has uncommitted changes.
- Aliases: `Eth-TrunkN` = `BondEthernetN`, `LoopBackN` = `loopN` (also `interface Eth-Trunk 0`,
  `interface LoopBack 0`). Physical ports and sub-interfaces keep their VPP names.
- Commands and interface names are case-insensitive, as in VRP (`dis int eth-trunk0`, `int CX5-0`);
  names are stored with their VPP spelling. The Cisco-style `rtr` stays case-sensitive.
- `display` instead of `show`, `undo` instead of `no`, `quit` goes up one view, `return` goes to the
  user view (refused with uncommitted changes).
- `commit`, `commit trial <seconds>`, `abort` (discard and return to user view), `abort trial`
  (roll back a trial commit now), `clear configuration candidate` (discard and stay).
- `ip address A.B.C.D <mask|length> [sub]`, `ip address unnumbered interface LoopBack0`,
  `ipv6 enable` (accepted, no-op), `ipv6 address X:X::X <length>`.
- Sub-interfaces: `dot1q termination vid <n>`, `qinq termination pe-vid <n> ce-vid <n>` (both exact
  match), `vlan-type dot1q <n>` (without exact match).
- Eth-Trunk: `mode lacp-static | manual load-balance`, `load-balance src-dst-mac | src-dst-ip |
  src-dst-ip-port`, `trunkport <port>`, or `eth-trunk <n>` in the member port's view.
- `display current-configuration`, `display configuration candidate [changes]`, `display this`,
  `display interface [name]`, `display ip interface brief`, `display ip routing-table statistics|<prefix>`,
  `display ipv6 routing-table ...`, `display eth-trunk`, `display bgp peer [<name> verbose]`, `display version`.
  `display ip routing-table` without arguments is not offered, because a full table would print a
  million lines.
- Extensions without a Huawei equivalent keep VPP terms: `lcp`, `l2xc`, `sflow enable`, bond modes
  `active-backup`, `round-robin`, `broadcast`.

### Commit semantics

- `commit` validates (vppcfg check + plan against VPP) and applies. If vppcfg rejects the
  configuration nothing changes and the reason is shown.
- `commit confirmed <minutes>` (`commit trial <seconds>` in `vrp`) applies provisionally. Confirm with
  `commit` (from any session) before the timeout, otherwise the configuration rolls back
  automatically. This also holds if the SSH session drops. `commit cancel` (`abort trial`) rolls back
  immediately and also resets the candidate.
- `abort` discards the candidate changes.
- The candidate is shared by all sessions. `configure` warns when it already contains changes (from
  another session, or left over from a rolled-back confirmed commit: Clixon restores running but does
  not clean the candidate).

## Implementation notes

- `clicon_rpc_commit()` in Clixon 7.9 does not send `<persist-id>` for a commit without
  `<confirmed/>`, so a persistent confirmed commit could never be confirmed. The CLI plugin sends its
  own commit RPC (`rpc_commit_persist_id`).
- A commit that confirms a confirmed commit does not apply candidate changes made after it;
  `vc_commit` performs a second commit when changes remain.
- CLIgen variables with an expand function are strict (only expanded values are accepted); commands
  that accept new names use `(<x:string expand()>("..")|<x:string>(".."))`.
- `rtr` runs `clixon_cli -l s`: Clixon's own timestamped log goes to syslog, and the plugin prints
  errors to the terminal as `% ...` (the `ca_errmsg` callback).

## Roadmap

- BIRD/BGP model (peers, ASN, prefixes, filters) rendered to `bird.conf`.
- `show commit history` and `rollback <n>` from `/var/lib/clixon-vppcfg/history`.
- Remaining vppcfg sections: bridgedomains, vxlan_tunnels, taps, prefixlists, acls, sflow.
- RESTCONF and NACM (users/roles).

## Credits

This project is a thin layer; the heavy lifting is done by these projects and their authors:

- **[Clixon](https://github.com/clicon/clixon)** and **[CLIgen](https://github.com/clicon/cligen)** by
  Olof Hagsand and contributors (with Benny Holmgren and Rubicon Communications, LLC / Netgate),
  dual licensed Apache-2.0 / GPL-3.0. They provide the YANG datastores, transactions, NETCONF and the
  CLI framework.
- **[vppcfg](https://git.ipng.ch/ipng/vppcfg)** by [Pim van Pelt](https://github.com/pimvanpelt) / IPng Networks, Apache-2.0. It
  validates the configuration and computes and applies the changes to VPP. The YANG model in this
  repository mirrors its YAML schema.
- **[VPP](https://fd.io/)** (Vector Packet Processing) by the FD.io project, Apache-2.0, including the
  linux-cp plugins.
- **[BIRD](https://bird.network.cz/)** by CZ.NIC, used by the `show bgp` commands.
- The architecture (Clixon as the management plane for VPP) follows the approach of
  [Netgate TNSR](https://www.tnsr.com/).
- An earlier prototype that talked to VPP directly: [clixon-vpp-control-plane](https://github.com/najiebhaswell/clixon-vpp-control-plane).

Developed with assistance from Claude (Anthropic).

## License

Apache License 2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
