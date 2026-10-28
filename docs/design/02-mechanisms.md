# 2. Mechanism mapping

| Menu item | Component | Justification from Rosalie's needs |
|---|---|---|
| **E** multi-process + supervisor | `sensord-*`, `hub`, `supervisor` | A dead sensor must never look like "humidity is fine". Each sensor runs in its own process, so a hung DHT11 read cannot stall light sampling or the dashboard. When a sensord dies, its connection hits EOF and the hub marks that source STALE immediately; after 60 s of silence it is DEAD. The supervisor restarts it and the gap is visible in the log as a `seq` jump. The dashboard shows STALE or DEAD instead of the last value. |
| **D** crash-consistent storage | `store` | The drift model needs days of history, and Rosalie's question after a bad night is "what happened", so the log must survive a power cut. Append-only fixed-size CRC records plus a recovery scan that truncates a torn tail. |

**Not chosen, and why.** A (kernel driver) adds no value for a slow sensor. B and C do not matter at DHT11 timescales: a few ms of jitter is irrelevant to humidity that moves over minutes. F has no rate pressure. We say so plainly rather than bolt on a mechanism. Both chosen mechanisms are written in C by us and are measured in the evaluation ([page 6](06-evaluation.md)).
