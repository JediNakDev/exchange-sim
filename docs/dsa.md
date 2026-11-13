# Data structures and algorithms

The matching engine owns exchange rules. Each instrument uses an order book to store resting orders, price levels, and FIFO links. Both run in C++ without sockets, clocks, or randomness.

## Price ladders

Each side has an array indexed by price tick. A slot points to a price level. Each level stores its total quantity, order count, and a doubly linked list of orders.

This gives direct access to a price level and constant-time FIFO append and unlink. Cached best prices make best bid and ask queries constant time.

A bitmap marks occupied ticks. Summary layers above it mark nonzero 64-bit words, up to a single top word. When a level empties, the book finds the next occupied price by reading at most two words per layer, so a search takes O(log64 P) words however sparse the band is. Each side also keeps its occupied-level count and the sum of its resting leaves, so those reads are constant time.

The tradeoff is memory. Arrays cover the configured price band, including empty ticks. The production engine allows at most 2^24 ticks per instrument side. A narrow band saves memory.

See [price levels](../lib/include/exchange/price_level.hpp), [order book](../lib/include/exchange/order_book.hpp), [occupancy bitmap](../lib/include/exchange/active_bitmap.hpp), and [instrument books](../lib/src/engine/price_ladder.hpp).

## Order lookup and storage

An open-addressing hash map maps order IDs to order pointers. It uses linear probing, a power-of-two table, and backward-shift deletion. The table reserves space for at most 50% load.

Lookup, insertion, and deletion take expected constant time. Collisions can make them linear in the table size. They are not worst-case constant time.

Free-list pools store orders and price levels. The production engine reserves them before trading and disables growth. A free slot can be taken or returned in constant time without a heap allocation.

The engine's instrument books share one order pool, one level pool, and one order ID map, each sized by the engine's order limit. Every live level holds at least one live order, so the order limit also bounds levels. Each book only sees and changes the orders resting on its own ladders.

The standalone order book can allow growth for replay. That mode can allocate pools, rehash the map, and expand ladders while processing orders.

See [hash map](../lib/include/exchange/flat_hash_map.hpp) and [object pool](../lib/include/exchange/object_pool.hpp).

## Matching

Continuous trading walks the opposite side from its best price. At each price, it fills the oldest resting order first. It stops when the incoming order is filled or no longer crosses.

The engine adds validation, self-trade prevention, time-in-force rules, and private and public events. It uses the book's passive storage operations so the book does not run a second matching pass. It walks the book's orders by pointer and executes, amends, or removes them in place, with no ID lookup.

A simple aggressive order takes expected O(1 + F + L log64 P) work, where F is the number of resting orders filled and L the price levels it empties. Rules that inspect liquidity or remove other orders add their own traversal work.

Auction planning scans occupied prices and bitmap words in the crossed range. It chooses the largest executable quantity, then the smallest imbalance, then the price closest to the last trade or reference price. A remaining tie uses the lower price. Execution fills FIFO orders at that single auction price.

See [matching engine](../lib/src/engine/matching_engine.cpp).

## Exchange metadata

Preallocated arrays hold frequently used order fields separately from session and query metadata. A hash map finds an order by session and client order ID.

Intrusive lists track active orders globally, by session, and by instrument. This lets disconnect handling visit a session's orders without scanning every slot. Bulk cancellation still takes work proportional to the orders visited.

A hash map finds an instrument by its symbol.

Event buffers are supplied by the caller. Their capacity is checked before commands run. Capacity and price limits bound memory use; they do not make every command constant time. Order storage grows with the order limit only, not with the number of instruments. Each instrument still reserves two price ladders and their bitmaps for its whole price band, so wide bands across many instruments can use substantial RAM.

See [order metadata](../lib/src/engine/order_metadata.hpp) and [engine interface](../lib/include/matching_engine.hpp).

## Operation time complexity

These costs apply after setup with growth disabled. Hash-table operations use expected costs; a collision chain can take O(H), where H is the number of slots in that table.

- I: configured instruments.
- N: live orders.
- P: ticks in one instrument's price band.
- B: bitmap words in that band, ceil(P / 64).
- W: bitmap words scanned word by word in a price range (auction planning only).
- F: resting orders filled.
- K: orders visited or returned.
- L: price levels visited or returned.

### Order book

| Operation | Expected time | Details |
| --- | --- | --- |
| Find an order by ID | O(1) | Worst case O(H) |
| Read a price level | O(1) | Direct tick lookup |
| Read best bid, ask, spread, mid-price, or top quantity | O(1) | Cached best levels |
| Read total order count | O(1) | Stored per-book counter |
| Read one side's total resting leaves | O(1) | Stored per-side sum |
| Append or unlink a known order at a level | O(1) | Intrusive FIFO links |
| Take or return a reserved pool slot | O(1) | Free-list head |
| Add a non-crossing order | O(1) | Hash insertion, pool slot, and FIFO append |
| Execute, amend, or remove a known order | O(1 + log64 P) | Pointer operation; the search runs only when the best level empties |
| Add an aggressive order | O(1 + F + L log64 P) | L is the levels emptied |
| Cancel an order | O(1 + log64 P) | The search runs only when the best level empties |
| Modify quantity | O(1 + log64 P) | Removing the order can empty the best level |
| Count occupied bid or ask levels | O(1) | Stored bitmap count |
| Find the next occupied price | O(log64 P) | At most two words per summary layer |
| Read a depth snapshot | O(1 + L log64 P) | Walks levels and allocates result vectors |

The standalone add path can cancel an unfilled remainder when its fill buffer or storage is exhausted. Growth-enabled replay can allocate, rehash, or resize a ladder, so it does not have the bounded-path costs above.

### Matching engine

| Operation | Expected time | Details |
| --- | --- | --- |
| Find an instrument by ID, or read its phase or best price | O(1) | Instrument array lookup |
| Find an instrument by symbol | O(1) | Symbol hash map |
| Find an order by session and client order ID | O(1) | Session-token hash map |
| Find an order by exchange order ID | O(1) | One order ID map shared by every instrument |
| Read live order count | O(1) | Stored counter |
| Read depth into a caller buffer | O(1 + L log64 P) | No result-vector allocation |
| Read orders at one price | O(1 + K) | Walks that level's FIFO |
| Visit all live orders | O(N) | Excludes callback work |
| Cancel a session's orders | O(1 + K log64 P) | Visits that session's list |
| Match a simple aggressive order | O(1 + F + L log64 P) | Excludes extra rule-specific traversals |
| Plan a crossed auction | O(1 + L + W) | Scans the occupancy words of the crossed range |
| Query auction indication | O(1 + L + W) when crossed; O(1) otherwise | The uncrossed case reads the stored side totals |

Order lookup by ID and symbol lookup can pay collision-chain costs. Removing an order from the shared ID map can also pay them.

FOK checks, self-trade prevention, auction execution, expiry, and bulk commands can inspect or remove more orders than an ordinary fill. Their cost depends on all orders and prices visited, not just fills produced. Callback and network serialization costs are separate.

## Network history and journals

Market-data recovery uses a bounded retransmission ring. Old messages are overwritten, so recovery only covers retained history. It cannot supply a full book snapshot to a late subscriber.

Standalone journaling passes commands to a writer through a bounded single-producer, single-consumer queue. It keeps disk writes outside matching. Managed exchanges currently reject journal configuration.

See [retransmission ring](../lib/include/md/retransmission_ring.hpp) and [journal worker](../lib/include/server/runtime/journal_worker.hpp).
