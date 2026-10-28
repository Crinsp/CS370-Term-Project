# 7. Ownership map and AI-use plan

Contributions so far: Gerritt set up the repo, deploy and test scaffolding, the fake sensor code, and wired and configured the sensors on the Pi. Zoe drove the project design, including choosing the two-sensor idea, and wrote the README contributions. Ownership means first authorship and answerability, not exclusivity **[proposed]**.

| Subsystem | Owner |
|---|---|
| sensord-dht, sensord-light | Zoe |
| hub: the pipeline that fuses both sensors (merge, liveness, status snapshot, forwarding to store) | Gerritt |
| record format (`src/common/record.h`) | Zoe |
| store (format, recovery) | Zoe |
| analysis (drift model) | TBD |
| supervisor, dashboard | shared |

## AI-use plan

Claude Code is used for exploring, drafting from approved plans, tests, and adversarial review of diffs. It is not used for the drift model's design decisions, the failure-mode table claims, or anything at runtime. The boundary stays visible in the repository: no network client in `src/` (the only socket is the LAN listener, and a CI grep (planned) will check for `connect(`, curl and HTTP-client usage), and no model files in the tree. Hard constraints live in CLAUDE.md, which both partners maintain.
