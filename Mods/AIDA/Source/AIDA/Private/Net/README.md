# Net/

Replication surface. RPC endpoints and the streaming chunk protocol.

- **AIDAPlayerComponent** — client↔server RPC surface per player (`ServerSendChat`, `ServerApproveProposal`, `ServerAddNote`, …).
- **Chunk protocol** — `FAIDAChunk` / `Multicast_MsgBegin` / `Multicast_MsgChunk` / `Multicast_MsgEnd`. Messages replicate as *events*, not one growing replicated string. Client assembles by `Seq`, verifies hash on `MsgEnd`, refetches on gap/mismatch/late-join.

See `docs/ARCHITECTURE.md` §5.
