# doasedit privilege boundary

OpenBSD `doas.conf` authorizes each operation. The current model assumes an
account already trusted to administer the machine, trusted directory ancestry
for the target and scratch paths, and serialization with other administrators
editing the same file. The startup `dd` call checks that particular invocation.
Later privileged read and publication each require their own authorization.
Exact-argument rules are awkward for an installation source whose name is
chosen at run time.

The calling process checks file identity before invoking doas. Authentication
can intervene before the privileged command resolves its operands again.
Rename-based publication for stable regular files gives a single namespace
update. Crash durability, compare-and-replace against an independent writer,
and general metadata preservation require separate guarantees.

## Design decision

Retain the trusted-administrator model for the current interface. A deployment
that needs narrow delegation would require a separate policy review and a
purpose-specific authorized operation. Such an operation would need a
root-owned policy describing allowed destinations and metadata, verified
source and destination identity within the authorized process, and trusted
directory ancestry. Define creation as exclusive and spell out how concurrent
replacement is handled. Audit special permission bits before accepting
user-provided content and define which stages preserve the edited copy on
failure.

A privileged helper would add an installed binary and require migrating doas
rules. Before adopting it, review the filesystem guarantees on OpenBSD and
validate policy, publication and recovery in a disposable VM. The present
code continues to use the existing binaries and doas rules.
