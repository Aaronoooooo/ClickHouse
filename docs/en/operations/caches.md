---
toc_priority: 65
toc_title: Caches
---

# Cache Types {#cache-types}

When performing queries, ClichHouse uses different caches.

Main cache types:
- `mark_cache` — Cache of marks used by table engines of the [MergeTree](../engines/table-engines/mergetree-family/mergetree.md) family.
- `uncompressed_cache` — Cache of uncompressed data used by table engines of the [MergeTree](../engines/table-engines/mergetree-family/mergetree.md) family.

Additional cache types:
- DNS cache
- [regexp](../interfaces/formats.md#data-format-regexp) cache
- compiled expressions cache
- [Avro format](../interfaces/formats.md#data-format-avro) schemas cache
- [dictionaries data cache](../sql-reference/dictionaries/index.md)

Indirectly used:
- OS page cache

To drop cache, use [SYSTEM DROP ... CACHE](../sql-reference/statements/system.md) statements.

[Original article](https://clickhouse.tech/docs/en/operations/caches/) <!--hide-->