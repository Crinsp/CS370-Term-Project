# 6. Evaluation plan

| Measurement | Method | Target |
|---|---|---|
| Sample-to-log latency distribution | timestamp at sensord read vs at log append; p50/p95/p99, idle and under CPU stress | p99 under 250 ms (to confirm) |
| Per-process CPU and RSS | heartbeat series over the 48 h soak | RSS flat (no leak), CPU under 10% |
| Domain metric: band-exit prediction | N provoked episodes labeled by us; drift model vs a plain threshold on lead time, misses and false alarms | model alerts earlier than the threshold with no more false alarms |
| Fault injection (mechanism E) | `kill -9` each child, unplug each sensor 10 min, power cut mid-write | each row in [section 3](03-failure-modes.md) produces its log line and recovers |
| Durability (mechanism D) | cut power at random during writes, repeat M times | zero corrupt records read back; loss window within 5 s |
