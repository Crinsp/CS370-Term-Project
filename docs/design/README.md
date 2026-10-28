# M2 Design Document: Gecko Enclosure Humidity Advisor

CS 370 Term Project. Authors: Gerritt Ewing, Zoe Ebert. Status: draft, snapshot of 2026-10-28.

The same content is available as a PDF: [`../M2_DesignDoc.pdf`](../M2_DesignDoc.pdf). The working draft that the team edits is [`../DESIGN.md`](../DESIGN.md); these pages are a readable copy of the PDF snapshot and may lag behind it.

Items marked **[Pi: unverified]** still need real evidence from the Pi. Items marked **[proposed]** await confirmation from both partners.

**User:** Rosalie Endris, keeper of a gecko. The device is advisory only: it senses, logs, analyzes and displays on a LAN dashboard. It never controls the humidifier, and it works with the network cable unplugged. Two sensors cooperate in one pipeline: a DHT11 (temperature and humidity, GPIO) and a TSL2591 (light, I2C). They run at different rates and interfaces, and the hub reconciles them into one fused snapshot.

## Pages

1. [Architecture](01-architecture.md): diagram with rates, IPC contract, liveness states
2. [Mechanism mapping](02-mechanisms.md): menu items E and D
3. [Failure-mode table](03-failure-modes.md)
4. [Storage and data](04-storage.md): record layout, rates, retention, power loss
5. [Constraints and substitutions](05-constraints.md), with the Pi evidence for both sensors
6. [Evaluation plan](06-evaluation.md)
7. [Ownership map and AI-use plan](07-ownership-ai.md)
