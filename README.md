# low-latency-orderbook

**LOB** is a single-threaded-hot-path, zero-allocation limit order book and
matching engine in C++20. It matches orders with price-time priority behind
lock-free SPSC ingress/egress rings, backs its book with a cache-friendly
banded price ladder over an arena-allocated order pool (no heap allocation
after startup), and ships an async binary logger plus a reproducible benchmark
suite. Correctness is established by differential testing against a naive
`std::map`-based reference book across millions of seeded random commands, with
ASan/UBSan/TSan-clean CI.

> Status: under active construction. See [DESIGN.md](DESIGN.md) for the
> architecture and [PLAN.md](PLAN.md) for the build plan. A full write-up with
> measured benchmark numbers lands with v1.0.

## License

[MIT](LICENSE)
