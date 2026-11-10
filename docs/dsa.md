# Data structures and algorithms

The matching engine owns exchange rules. Each instrument uses an order book to store resting orders, price levels, and FIFO links. Both run in C++ without sockets, clocks, or randomness.

## Price ladders

Each side has an array indexed by price tick. A slot points to a price level. Each level stores its total quantity, order count, and a doubly linked list of orders.

This gives direct access to a price level and constant-time FIFO append and unlink. Cached best prices make best bid and ask queries constant time.

A bitmap marks occupied ticks. When a level empties, the book scans 64-bit words to find the next occupied price. Sparse price bands can need several word scans.

The tradeoff is memory. Arrays cover the configured price band, including empty ticks. The production engine allows at most 2^24 ticks per instrument side. A narrow band saves memory.

See [price levels](../lib/include/exchange/price_level.hpp), [order book](../lib/include/exchange/order_book.hpp), and [instrument ladders](../lib/src/exsim/engine/price_ladder.hpp).

## Order lookup and storage

An open-addressing hash map maps order IDs to order pointers. It uses linear probing, a power-of-two table, and backward-shift deletion. The table reserves space for at most 50% load.

Lookup, insertion, and deletion take expected constant time. Collisions can make them linear in the table size. They are not worst-case constant time.

Free-list pools store orders and price levels. The production engine reserves them before trading and disables growth. A free slot can be taken or returned in constant time without a heap allocation.

The standalone order book can allow growth for replay. That mode can allocate pools, rehash the map, and expand ladders while processing orders.

See [hash map](../lib/include/exchange/flat_hash_map.hpp) and [object pool](../lib/include/exchange/object_pool.hpp).

## Matching

Continuous trading walks the opposite side from its best price. At each price, it fills the oldest resting order first. It stops when the incoming order is filled or no longer crosses.

The engine adds validation, self-trade prevention, time-in-force rules, and private and public events. It uses the book's passive storage operations so the book does not run a second matching pass.

A simple aggressive order takes expected O(F + W) work, where F is the number of resting orders filled and W is the occupancy words scanned. Rules that inspect liquidity or remove other orders add their own traversal work.

Auction planning scans occupied prices and bitmap words in the crossed range. It chooses the largest executable quantity, then the smallest imbalance, then the price closest to the last trade or reference price. A remaining tie uses the lower price. Execution fills FIFO orders at that single auction price.

See [matching engine](../lib/src/exsim/engine/matching_engine.cpp).

## Exchange metadata

Preallocated arrays hold frequently used order fields separately from session and query metadata. A hash map finds an order by session and client order ID.

Intrusive lists track active orders globally, by session, and by instrument. This lets disconnect handling visit a session's orders without scanning every slot. Bulk cancellation still takes work proportional to the orders visited.

Event buffers are supplied by the caller. Their capacity is checked before commands run. Capacity and price limits bound memory use; they do not make every command constant time. Each instrument also reserves book storage, so increasing instrument and order limits can use substantial RAM.

See [order metadata](../lib/src/exsim/engine/order_metadata.hpp) and [engine interface](../lib/include/matching_engine.hpp).

## Network history and journals

Market-data recovery uses a bounded retransmission ring. Old messages are overwritten, so recovery only covers retained history. It cannot supply a full book snapshot to a late subscriber.

Standalone journaling passes commands to a writer through a bounded single-producer, single-consumer queue. It keeps disk writes outside matching. Managed exchanges currently reject journal configuration.

See [retransmission ring](../lib/include/md/retransmission_ring.hpp) and [journal worker](../lib/include/server/runtime/journal_worker.hpp).
