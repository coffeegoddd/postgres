"""Registries: implementations of each harness interface, by name.

Implementations register themselves with a decorator when their module is
imported; load_all() imports every module in the implementation packages,
so a new file is picked up without touching the driver.
"""

from __future__ import annotations

import importlib
import pkgutil
from typing import Generic, TypeVar

T = TypeVar("T")


class Registry(Generic[T]):
    def __init__(self, kind: str):
        self.kind = kind
        self._items: dict[str, type[T]] = {}

    def register(self, name: str):
        def deco(cls: type[T]) -> type[T]:
            if name in self._items:
                raise ValueError(f"duplicate {self.kind} {name!r}")
            self._items[name] = cls
            return cls
        return deco

    def get(self, name: str) -> type[T]:
        try:
            return self._items[name]
        except KeyError:
            known = ", ".join(sorted(self._items)) or "none"
            raise KeyError(f"unknown {self.kind} {name!r} (known: {known})") from None

    def names(self) -> list[str]:
        return sorted(self._items)

    def items(self) -> list[tuple[str, type[T]]]:
        return sorted(self._items.items())


workloads: Registry = Registry("workload")
collectors: Registry = Registry("collector")
sinks: Registry = Registry("sink")
clusters: Registry = Registry("cluster provider")

_PACKAGES = ("pbench.workloads", "pbench.collectors", "pbench.sinks",
             "pbench.clusters")
_loaded = False


def load_all() -> None:
    global _loaded
    if _loaded:
        return
    for pkg_name in _PACKAGES:
        pkg = importlib.import_module(pkg_name)
        for mod in pkgutil.iter_modules(pkg.__path__):
            if not mod.name.startswith("_"):
                importlib.import_module(f"{pkg_name}.{mod.name}")
    _loaded = True
