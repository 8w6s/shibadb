
from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path
from typing import Callable, Iterable, Sequence

__version__ = "0.1.0"
ABI_VERSION = 1

SDB_SYNCHRONOUS_FULL = 0
SDB_SYNCHRONOUS_NORMAL = 1
SDB_CACHE_AUTO = 0xFFFFFFFFFFFFFFFF

_u8 = ctypes.c_uint8
_u8_p = ctypes.POINTER(_u8)
_database_p = ctypes.c_void_p
_transaction_p = ctypes.c_void_p

class Error(RuntimeError):

    def __init__(self, status: int, message: str):
        super().__init__(f"{message} (status={status})")
        self.status = status

class NotFoundError(Error):
    pass

class ConflictError(Error):
    pass

class BusyError(Error):
    pass

class AuthenticationError(Error):
    pass

class CorruptionError(Error):
    pass

class _Options(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("page_size", ctypes.c_uint32),
        ("kdf_iterations", ctypes.c_uint32),
        ("password", _u8_p),
        ("password_size", ctypes.c_size_t),
        ("cache_bytes", ctypes.c_uint64),
        ("synchronous", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 2),
    ]

class _IndexTerm(ctypes.Structure):
    _fields_ = [
        ("index_name", _u8_p),
        ("index_name_size", ctypes.c_size_t),
        ("value", _u8_p),
        ("value_size", ctypes.c_size_t),
    ]

class _VerifyResult(ctypes.Structure):
    _fields_ = [
        ("allocated_page_count", ctypes.c_uint64),
        ("free_page_count", ctypes.c_uint64),
        ("btree_node_count", ctypes.c_uint64),
        ("btree_leaf_count", ctypes.c_uint64),
        ("raw_entry_count", ctypes.c_uint64),
        ("object_count", ctypes.c_uint64),
        ("live_chunk_count", ctypes.c_uint64),
        ("stale_entry_count", ctypes.c_uint64),
        ("logical_byte_count", ctypes.c_uint64),
        ("btree_height", ctypes.c_uint32),
        ("reserved_alignment", ctypes.c_uint32),
        ("reserved", ctypes.c_uint64 * 3),
    ]

class _BackupResult(ctypes.Structure):
    _fields_ = [
        ("byte_count", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
    ]

class _CompactResult(ctypes.Structure):
    _fields_ = [
        ("byte_count_before", ctypes.c_uint64),
        ("byte_count_after", ctypes.c_uint64),
        ("raw_entries_before", ctypes.c_uint64),
        ("raw_entries_after", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
    ]

class _InfoResult(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("page_size", ctypes.c_uint32),
        ("encrypted", ctypes.c_bool),
        ("reserved_alignment", ctypes.c_uint8 * 3),
        ("kdf_iterations", ctypes.c_uint32),
        ("generation", ctypes.c_uint64),
        ("checkpoint_lsn", ctypes.c_uint64),
        ("page_count", ctypes.c_uint64),
        ("reserved", ctypes.c_uint64 * 4),
    ]

_VISITOR = ctypes.CFUNCTYPE(
    ctypes.c_bool, ctypes.c_void_p, _u8_p, ctypes.c_size_t
)

_SCAN_VISITOR = ctypes.CFUNCTYPE(
    ctypes.c_bool, ctypes.c_void_p,
    _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
)

_NS_VISITOR = ctypes.CFUNCTYPE(
    ctypes.c_bool, ctypes.c_void_p,
    ctypes.c_uint16, _u8_p, ctypes.c_size_t,
)

def _load_library() -> ctypes.CDLL:
    explicit = os.environ.get("SHIBADB_LIBRARY")
    candidates = [explicit] if explicit else []
    found = ctypes.util.find_library("shibadb")
    if found:
        candidates.append(found)
    package_dir = Path(__file__).resolve().parent
    candidates.extend(
        str(package_dir / name)
        for name in ("libshibadb.so.1", "libshibadb.dylib", "shibadb.dll")
    )
    errors = []
    for candidate in candidates:
        if not candidate:
            continue
        try:
            return ctypes.CDLL(candidate)
        except OSError as error:
            errors.append(f"{candidate}: {error}")
    detail = "; ".join(errors) if errors else "no candidate library found"
    raise ImportError(
        "Unable to load ShibaDB native library. Install libshibadb or set "
        f"SHIBADB_LIBRARY. {detail}"
    )

_lib = _load_library()

_lib.sdb_abi_version.argtypes = []
_lib.sdb_abi_version.restype = ctypes.c_uint32
_lib.sdb_version_string.argtypes = []
_lib.sdb_version_string.restype = ctypes.c_char_p
_lib.sdb_status_string.argtypes = [ctypes.c_int]
_lib.sdb_status_string.restype = ctypes.c_char_p
_lib.sdb_database_options_init.argtypes = [ctypes.POINTER(_Options)]
_lib.sdb_database_options_init.restype = None
_lib.sdb_database_create.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(_Options), ctypes.POINTER(_database_p)
]
_lib.sdb_database_create.restype = ctypes.c_int
_lib.sdb_database_open.argtypes = list(_lib.sdb_database_create.argtypes)
_lib.sdb_database_open.restype = ctypes.c_int
_lib.sdb_database_close.argtypes = [_database_p]
_lib.sdb_database_close.restype = ctypes.c_int
_lib.sdb_transaction_begin.argtypes = [
    _database_p, ctypes.POINTER(_transaction_p)
]
_lib.sdb_transaction_begin.restype = ctypes.c_int
for _name in ("commit", "rollback", "close"):
    _function = getattr(_lib, f"sdb_transaction_{_name}")
    _function.argtypes = [_transaction_p]
    _function.restype = ctypes.c_int

for _name in ("kv", "blob"):
    _put = getattr(_lib, f"sdb_{_name}_put")
    _put.argtypes = [
        _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
        _u8_p, ctypes.c_size_t,
    ]
    _put.restype = ctypes.c_int
    _get = getattr(_lib, f"sdb_{_name}_get")
    _get.argtypes = [
        _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
        _u8_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    _get.restype = ctypes.c_int
    _delete = getattr(_lib, f"sdb_{_name}_delete")
    _delete.argtypes = [
        _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ]
    _delete.restype = ctypes.c_int
    _transaction_put = getattr(_lib, f"sdb_transaction_{_name}_put")
    _transaction_put.argtypes = [
        _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
        _u8_p, ctypes.c_size_t,
    ]
    _transaction_put.restype = ctypes.c_int
    _transaction_get = getattr(_lib, f"sdb_transaction_{_name}_get")
    _transaction_get.argtypes = [
        _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
        _u8_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    _transaction_get.restype = ctypes.c_int
    _transaction_delete = getattr(
        _lib, f"sdb_transaction_{_name}_delete"
    )
    _transaction_delete.argtypes = [
        _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ]
    _transaction_delete.restype = ctypes.c_int

_lib.sdb_index_create.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.c_bool,
]
_lib.sdb_index_create.restype = ctypes.c_int
_lib.sdb_document_put.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, ctypes.POINTER(_IndexTerm), ctypes.c_size_t,
]
_lib.sdb_document_put.restype = ctypes.c_int
_lib.sdb_document_get.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
]
_lib.sdb_document_get.restype = ctypes.c_int
_lib.sdb_document_delete.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
]
_lib.sdb_document_delete.restype = ctypes.c_int
_lib.sdb_transaction_index_create.argtypes = [
    _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.c_bool,
]
_lib.sdb_transaction_index_create.restype = ctypes.c_int
_lib.sdb_transaction_document_put.argtypes = [
    _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, ctypes.POINTER(_IndexTerm), ctypes.c_size_t,
]
_lib.sdb_transaction_document_put.restype = ctypes.c_int
_lib.sdb_transaction_document_get.argtypes = [
    _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
]
_lib.sdb_transaction_document_get.restype = ctypes.c_int
_lib.sdb_transaction_document_delete.argtypes = [
    _transaction_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
]
_lib.sdb_transaction_document_delete.restype = ctypes.c_int
_lib.sdb_index_visit.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, _VISITOR, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.sdb_index_visit.restype = ctypes.c_int
_lib.sdb_database_verify.argtypes = [
    _database_p, ctypes.POINTER(_VerifyResult)
]
_lib.sdb_database_verify.restype = ctypes.c_int
_lib.sdb_database_backup.argtypes = [
    _database_p, ctypes.c_char_p, ctypes.c_bool,
    ctypes.POINTER(_BackupResult),
]
_lib.sdb_database_backup.restype = ctypes.c_int
_lib.sdb_database_compact.argtypes = [
    _database_p, ctypes.POINTER(_Options), ctypes.POINTER(_CompactResult)
]
_lib.sdb_database_compact.restype = ctypes.c_int
_lib.sdb_database_migrate.argtypes = [
    _database_p, ctypes.c_uint16, ctypes.POINTER(_Options),
    ctypes.POINTER(_CompactResult),
]
_lib.sdb_database_migrate.restype = ctypes.c_int
_lib.sdb_kv_scan_prefix.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.c_void_p, _SCAN_VISITOR, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.sdb_kv_scan_prefix.restype = ctypes.c_int
_lib.sdb_list_namespaces.argtypes = [
    _database_p, _NS_VISITOR, ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_size_t),
]
_lib.sdb_list_namespaces.restype = ctypes.c_int
_lib.sdb_database_migrate_file.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(_Options), ctypes.POINTER(_Options),
    ctypes.POINTER(_CompactResult),
]
_lib.sdb_database_migrate_file.restype = ctypes.c_int
_lib.sdb_database_info.argtypes = [
    _database_p, ctypes.POINTER(_InfoResult)
]
_lib.sdb_database_info.restype = ctypes.c_int
_lib.sdb_kv_exists.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_bool),
]
_lib.sdb_kv_exists.restype = ctypes.c_int
_lib.sdb_kv_count.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64),
]
_lib.sdb_kv_count.restype = ctypes.c_int
_lib.sdb_kv_count_prefix.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_uint64),
]
_lib.sdb_kv_count_prefix.restype = ctypes.c_int
_lib.sdb_kv_put_if_absent.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t,
]
_lib.sdb_kv_put_if_absent.restype = ctypes.c_int
_lib.sdb_kv_compare_and_swap.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
]
_lib.sdb_kv_compare_and_swap.restype = ctypes.c_int
_lib.sdb_kv_increment.argtypes = [
    _database_p, _u8_p, ctypes.c_size_t, _u8_p, ctypes.c_size_t,
    ctypes.c_int64, ctypes.POINTER(ctypes.c_int64),
]
_lib.sdb_kv_increment.restype = ctypes.c_int

def abi_version() -> int:
    return int(_lib.sdb_abi_version())

def version() -> str:
    return _lib.sdb_version_string().decode("ascii")

if abi_version() != ABI_VERSION:
    raise ImportError(
        f"ShibaDB ABI mismatch: binding={ABI_VERSION}, native={abi_version()}"
    )

def _raise_status(status: int) -> None:
    if status == 0:
        return
    message = _lib.sdb_status_string(status).decode("utf-8", "replace")
    error_type = {
        5: CorruptionError,
        10: NotFoundError,
        11: AuthenticationError,
        12: ConflictError,
        13: BusyError,
    }.get(status, Error)
    raise error_type(status, message)

def _bytes(value: bytes | bytearray | memoryview | str) -> bytes:
    if isinstance(value, str):
        return value.encode("utf-8")
    return bytes(value)

def _buffer(value: bytes) -> tuple[object | None, _u8_p]:
    if not value:
        return None, ctypes.cast(None, _u8_p)
    storage = (_u8 * len(value)).from_buffer_copy(value)
    return storage, ctypes.cast(storage, _u8_p)

def _path(path: os.PathLike[str] | str) -> bytes:
    return os.fsencode(os.fspath(path))


def _document_expired(document: dict, now: float) -> bool:
    """True iff the document carries a numeric "_expires_at" that is <= now."""
    expires_at = document.get("_expires_at")
    return isinstance(expires_at, (int, float)) and not isinstance(
        expires_at, bool
    ) and expires_at <= now

def _options(
    password: bytes | str | None,
    page_size: int,
    kdf_iterations: int,
    cache_bytes: int = 0,
    synchronous: int = SDB_SYNCHRONOUS_FULL,
) -> tuple[_Options, object | None]:
    options = _Options()
    _lib.sdb_database_options_init(ctypes.byref(options))
    options.page_size = page_size
    options.kdf_iterations = kdf_iterations
    options.cache_bytes = cache_bytes
    options.synchronous = synchronous
    password_bytes = b"" if password is None else _bytes(password)
    storage, pointer = _buffer(password_bytes)
    options.password = pointer
    options.password_size = len(password_bytes)
    return options, storage

def _dynamic_get(function, handle, *key_arguments) -> bytes:
    required = ctypes.c_size_t()
    status = function(
        handle,
        *key_arguments,
        ctypes.cast(None, _u8_p),
        0,
        ctypes.byref(required),
    )
    if status not in (0, 2):
        _raise_status(status)
    for _attempt in range(8):
        if required.value == 0:
            return b""
        output = (_u8 * required.value)()
        status = function(
            handle,
            *key_arguments,
            ctypes.cast(output, _u8_p),
            len(output),
            ctypes.byref(required),
        )
        if status == 0:
            return bytes(output[: required.value])
        if status != 2:
            _raise_status(status)
    _raise_status(2)
    raise AssertionError("unreachable")

class Database:

    def __init__(self, handle: _database_p):
        self._handle = handle

    @classmethod
    def create(
        cls,
        path: os.PathLike[str] | str,
        *,
        password: bytes | str | None = None,
        page_size: int = 4096,
        kdf_iterations: int = 600000,
        cache_bytes: int = 0,
        synchronous: int = SDB_SYNCHRONOUS_FULL,
    ) -> "Database":
        options, keepalive = _options(
            password, page_size, kdf_iterations, cache_bytes, synchronous
        )
        handle = _database_p()
        status = _lib.sdb_database_create(
            _path(path), ctypes.byref(options), ctypes.byref(handle)
        )
        del keepalive
        _raise_status(status)
        return cls(handle)

    @classmethod
    def open(
        cls,
        path: os.PathLike[str] | str,
        *,
        password: bytes | str | None = None,
        page_size: int = 4096,
        kdf_iterations: int = 600000,
        cache_bytes: int = 0,
        synchronous: int = SDB_SYNCHRONOUS_FULL,
    ) -> "Database":
        options, keepalive = _options(
            password, page_size, kdf_iterations, cache_bytes, synchronous
        )
        handle = _database_p()
        status = _lib.sdb_database_open(
            _path(path), ctypes.byref(options), ctypes.byref(handle)
        )
        del keepalive
        _raise_status(status)
        return cls(handle)

    def _require_open(self) -> _database_p:
        if not self._handle:
            raise RuntimeError("database is closed")
        return self._handle

    def close(self) -> None:
        if self._handle:
            status = _lib.sdb_database_close(self._handle)
            # sdb_database_close frees the C database on every path EXCEPT its
            # two early-return guards, which return BEFORE the point of no
            # return WITHOUT freeing:
            #   SDB_E_INVALID_ARGUMENT (1) - handle null / already closed
            #   SDB_E_BUSY (13)            - a transaction/callback/commit is
            #                                still in flight
            # On those the C database is still alive and still holding its
            # process/path file locks, so clearing the handle here would leak
            # it, strand the locks (blocking reopen), skip the close-time
            # checkpoint, and make the documented recovery (clear the cause,
            # retry close) impossible. Keep the handle on those; clear it on
            # success and on any late (post-checkpoint) error, where the C
            # side has already freed it and a retry would be a use-after-free.
            if status not in (1, 13):
                self._handle = _database_p()
            _raise_status(status)

    def __enter__(self) -> "Database":
        self._require_open()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def transaction(self) -> "Transaction":
        return Transaction._begin(self)

    def _put(self, kind: str, namespace, key, value) -> None:
        namespace_bytes, key_bytes, value_bytes = map(
            _bytes, (namespace, key, value)
        )
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        value_store, value_ptr = _buffer(value_bytes)
        status = getattr(_lib, f"sdb_{kind}_put")(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            value_ptr, len(value_bytes),
        )
        del ns_store, key_store, value_store
        _raise_status(status)

    def _get(self, kind: str, namespace, key) -> bytes:
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        function = getattr(_lib, f"sdb_{kind}_get")
        result = _dynamic_get(
            function,
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
        )
        del ns_store, key_store
        return result

    def _delete(self, kind: str, namespace, key) -> None:
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        status = getattr(_lib, f"sdb_{kind}_delete")(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
        )
        del ns_store, key_store
        _raise_status(status)

    def put(self, namespace, key, value) -> None:
        self._put("kv", namespace, key, value)

    def get(self, namespace, key) -> bytes:
        return self._get("kv", namespace, key)

    def delete(self, namespace, key) -> None:
        self._delete("kv", namespace, key)

    def put_blob(self, namespace, key, value) -> None:
        self._put("blob", namespace, key, value)

    def get_blob(self, namespace, key) -> bytes:
        return self._get("blob", namespace, key)

    def delete_blob(self, namespace, key) -> None:
        self._delete("blob", namespace, key)

    def exists(self, namespace, key) -> bool:
        """True iff a KV key is present, without copying its value."""
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        present = ctypes.c_bool()
        status = _lib.sdb_kv_exists(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            ctypes.byref(present),
        )
        del ns_store, key_store
        _raise_status(status)
        return bool(present.value)

    def count(self, namespace) -> int:
        """Count all keys in a KV namespace."""
        namespace_bytes = _bytes(namespace)
        ns_store, ns_ptr = _buffer(namespace_bytes)
        total = ctypes.c_uint64()
        status = _lib.sdb_kv_count(
            self._require_open(),
            ns_ptr, len(namespace_bytes), ctypes.byref(total),
        )
        del ns_store
        _raise_status(status)
        return int(total.value)

    def count_prefix(self, namespace, prefix=b"") -> int:
        """Count keys in a KV namespace whose key begins with prefix (an empty
        prefix counts the whole namespace)."""
        namespace_bytes, prefix_bytes = map(_bytes, (namespace, prefix))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        prefix_store, prefix_ptr = _buffer(prefix_bytes)
        total = ctypes.c_uint64()
        status = _lib.sdb_kv_count_prefix(
            self._require_open(),
            ns_ptr, len(namespace_bytes), prefix_ptr, len(prefix_bytes),
            ctypes.byref(total),
        )
        del ns_store, prefix_store
        _raise_status(status)
        return int(total.value)

    def put_if_absent(self, namespace, key, value) -> bool:
        """Atomically store value only if the key does not yet exist. Return
        True if it was written, False if the key already existed."""
        namespace_bytes, key_bytes, value_bytes = map(
            _bytes, (namespace, key, value)
        )
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        value_store, value_ptr = _buffer(value_bytes)
        status = _lib.sdb_kv_put_if_absent(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            value_ptr, len(value_bytes),
        )
        del ns_store, key_store, value_store
        if status == 12:
            return False
        _raise_status(status)
        return True

    def compare_and_swap(self, namespace, key, expected, desired) -> bool:
        """Atomically replace the value only if the current value equals
        expected. Return True on swap, False if the key is absent or the current
        value differs (nothing written)."""
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        expected_bytes, desired_bytes = map(_bytes, (expected, desired))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        expected_store, expected_ptr = _buffer(expected_bytes)
        desired_store, desired_ptr = _buffer(desired_bytes)
        status = _lib.sdb_kv_compare_and_swap(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            expected_ptr, len(expected_bytes),
            desired_ptr, len(desired_bytes),
        )
        del ns_store, key_store, expected_store, desired_store
        if status == 12:
            return False
        _raise_status(status)
        return True

    def increment(self, namespace, key, delta: int = 1) -> int:
        """Atomically add delta to an 8-byte little-endian counter and return
        the new value. A missing key is treated as 0 and created. Raises Error
        if the existing value is not exactly 8 bytes, or on signed overflow."""
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        new_value = ctypes.c_int64()
        status = _lib.sdb_kv_increment(
            self._require_open(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            ctypes.c_int64(delta), ctypes.byref(new_value),
        )
        del ns_store, key_store
        _raise_status(status)
        return int(new_value.value)

    def info(self) -> dict[str, int | bool]:
        """O(1) configuration snapshot read straight from the in-memory
        superblock: page_size, encrypted, kdf_iterations, generation,
        checkpoint_lsn, page_count. Unlike verify() it never walks the tree."""
        result = _InfoResult()
        result.struct_size = ctypes.sizeof(_InfoResult)
        _raise_status(_lib.sdb_database_info(
            self._require_open(), ctypes.byref(result)
        ))
        return {
            "page_size": int(result.page_size),
            "encrypted": bool(result.encrypted),
            "kdf_iterations": int(result.kdf_iterations),
            "generation": int(result.generation),
            "checkpoint_lsn": int(result.checkpoint_lsn),
            "page_count": int(result.page_count),
        }

    def create_index(self, collection, name, *, unique: bool = False) -> None:
        collection_bytes, name_bytes = map(_bytes, (collection, name))
        collection_store, collection_ptr = _buffer(collection_bytes)
        name_store, name_ptr = _buffer(name_bytes)
        status = _lib.sdb_index_create(
            self._require_open(),
            collection_ptr, len(collection_bytes),
            name_ptr, len(name_bytes), unique,
        )
        del collection_store, name_store
        _raise_status(status)

    def put_document(
        self,
        collection,
        document_id,
        document,
        *,
        terms: Sequence[tuple[bytes | str, bytes | str]] = (),
    ) -> None:
        collection_bytes, id_bytes, document_bytes = map(
            _bytes, (collection, document_id, document)
        )
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        document_store, document_ptr = _buffer(document_bytes)
        term_array = (_IndexTerm * len(terms))()
        term_keepalive = []
        for index, (name, value) in enumerate(terms):
            name_bytes, value_bytes = map(_bytes, (name, value))
            name_store, name_ptr = _buffer(name_bytes)
            value_store, value_ptr = _buffer(value_bytes)
            term_keepalive.extend((name_store, value_store))
            term_array[index] = _IndexTerm(
                name_ptr, len(name_bytes), value_ptr, len(value_bytes)
            )
        status = _lib.sdb_document_put(
            self._require_open(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
            document_ptr, len(document_bytes),
            term_array if terms else None, len(terms),
        )
        del collection_store, id_store, document_store, term_keepalive
        _raise_status(status)

    def get_document(self, collection, document_id) -> bytes:
        collection_bytes, id_bytes = map(_bytes, (collection, document_id))
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        result = _dynamic_get(
            _lib.sdb_document_get,
            self._require_open(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
        )
        del collection_store, id_store
        return result

    def delete_document(self, collection, document_id) -> None:
        collection_bytes, id_bytes = map(_bytes, (collection, document_id))
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        status = _lib.sdb_document_delete(
            self._require_open(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
        )
        del collection_store, id_store
        _raise_status(status)

    def find(self, collection, index_name, value) -> list[bytes]:
        collection_bytes, index_bytes, value_bytes = map(
            _bytes, (collection, index_name, value)
        )
        collection_store, collection_ptr = _buffer(collection_bytes)
        index_store, index_ptr = _buffer(index_bytes)
        value_store, value_ptr = _buffer(value_bytes)
        found: list[bytes] = []

        @_VISITOR
        def visitor(context, document_id, document_id_size):
            del context
            found.append(ctypes.string_at(document_id, document_id_size))
            return True

        match_count = ctypes.c_size_t()
        status = _lib.sdb_index_visit(
            self._require_open(),
            collection_ptr, len(collection_bytes),
            index_ptr, len(index_bytes), value_ptr, len(value_bytes),
            visitor, None, ctypes.byref(match_count),
        )
        del collection_store, index_store, value_store
        _raise_status(status)
        if match_count.value != len(found):
            raise RuntimeError("native index callback count mismatch")
        return found

    def scan(
        self, namespace, prefix=b"", *, reverse: bool = False
    ) -> list[tuple[bytes, bytes]]:
        """Return (key, value) pairs of the KV namespace whose key begins with
        prefix (an empty prefix scans the whole namespace), in ascending key
        order — or descending if reverse is set. The scan holds a read snapshot
        for its duration, so it excludes writers on this handle until it
        returns."""
        namespace_bytes = _bytes(namespace)
        prefix_bytes = _bytes(prefix)
        namespace_store, namespace_ptr = _buffer(namespace_bytes)
        prefix_store, prefix_ptr = _buffer(prefix_bytes)
        results: list[tuple[bytes, bytes]] = []

        @_SCAN_VISITOR
        def visitor(context, key, key_size, value, value_size):
            del context
            results.append((
                ctypes.string_at(key, key_size),
                ctypes.string_at(value, value_size),
            ))
            return True

        match_count = ctypes.c_size_t()
        status = _lib.sdb_kv_scan_prefix(
            self._require_open(),
            namespace_ptr, len(namespace_bytes),
            prefix_ptr, len(prefix_bytes),
            None, visitor, None, ctypes.byref(match_count),
        )
        del namespace_store, prefix_store
        _raise_status(status)
        if match_count.value != len(results):
            raise RuntimeError("native scan callback count mismatch")
        if reverse:
            results.reverse()
        return results

    def find_query(
        self, namespace, query: dict, *, prefix=b"", include_expired: bool = False
    ) -> list[tuple[bytes, dict]]:
        """Scan a KV namespace of JSON-encoded values and return (key, doc) for
        every value that decodes to a JSON object matching the Mongo-style
        query (see shibadb.query.matches_query). Values that are not JSON
        objects are skipped. A document whose "_expires_at" timestamp has passed
        is skipped too (TTL, see put_json), unless include_expired is set.
        Filtering is client-side over the ordered scan."""
        import json
        import time

        from .query import matches_query

        now = time.time()
        matches: list[tuple[bytes, dict]] = []
        for key, value in self.scan(namespace, prefix):
            try:
                document = json.loads(value)
            except (ValueError, UnicodeDecodeError):
                continue
            if not isinstance(document, dict):
                continue
            if not include_expired and _document_expired(document, now):
                continue
            if matches_query(document, query):
                matches.append((key, document))
        return matches

    def put_json(self, namespace, key, document: dict, *, ttl=None) -> None:
        """Store a JSON document. If ttl (seconds) is given, stamp it with an
        "_expires_at" absolute timestamp so find_query/get_json treat it as gone
        once that time passes (lazy expiry; call purge_expired to reclaim)."""
        import json
        import time

        if not isinstance(document, dict):
            raise TypeError("put_json requires a dict document")
        if ttl is not None:
            document = {**document, "_expires_at": time.time() + float(ttl)}
        self.put(namespace, key, json.dumps(document))

    def get_json(self, namespace, key, *, include_expired: bool = False) -> dict:
        """Get a JSON document. Raises NotFoundError if it is missing or (unless
        include_expired) its "_expires_at" TTL has passed."""
        import json
        import time

        document = json.loads(self.get(namespace, key))
        if (not include_expired and isinstance(document, dict)
                and _document_expired(document, time.time())):
            raise NotFoundError(10, "document has expired")
        return document

    def purge_expired(self, namespace) -> int:
        """Delete every JSON document in the namespace whose "_expires_at" TTL
        has passed, and return the number removed (a TTL reaper)."""
        import json
        import time

        now = time.time()
        removed = 0
        for key, value in self.scan(namespace):
            try:
                document = json.loads(value)
            except (ValueError, UnicodeDecodeError):
                continue
            if isinstance(document, dict) and _document_expired(document, now):
                self.delete(namespace, key)
                removed += 1
        return removed

    def resolve_ref(self, ref: dict, *, as_json: bool = True):
        """Resolve a DBRef {"$ref": namespace, "$id": key} to its KV value,
        or None if the reference is malformed or the target is missing. With
        as_json (default) the value is JSON-decoded; otherwise raw bytes are
        returned."""
        if not isinstance(ref, dict) or "$ref" not in ref or "$id" not in ref:
            return None
        try:
            value = self.get(ref["$ref"], ref["$id"])
        except NotFoundError:
            return None
        if as_json:
            import json

            return json.loads(value)
        return value

    def list_namespaces(self) -> list[tuple[int, bytes]]:
        """Return the (kind, namespace) pairs that currently hold data, each
        once, in on-disk order. kind is 1=KV, 2=blob, 3=document."""
        results: list[tuple[int, bytes]] = []

        @_NS_VISITOR
        def visitor(context, kind, name, name_size):
            del context
            results.append((int(kind), ctypes.string_at(name, name_size)))
            return True

        count = ctypes.c_size_t()
        status = _lib.sdb_list_namespaces(
            self._require_open(), visitor, None, ctypes.byref(count)
        )
        _raise_status(status)
        if count.value != len(results):
            raise RuntimeError("native namespace callback count mismatch")
        return results

    def query(self, statement: str):
        """Execute a SQL statement over a KV namespace of JSON documents, using
        each document's "_id" field as its key. Return value depends on the op:
          SELECT -> list[dict] of matching documents (WHERE / ORDER BY [ASC|DESC]
                    / LIMIT / OFFSET supported);
          INSERT -> the key (bytes) of the stored row (requires an "_id" field);
          UPDATE -> int count of rows updated (SET fields merged into each match);
          DELETE -> int count of rows deleted.
        Non-SQL dialects still parse via shibadb.polyglot.PolyglotParser but are
        not executed here."""
        import json

        from .polyglot import PolyglotParser, where_to_query

        plan = PolyglotParser().parse(statement)
        if plan is None:
            raise ValueError(f"could not parse query: {statement!r}")
        if plan.get("lang") == "mongo":
            return self._query_mongo(plan)
        if plan.get("lang") != "sql":
            raise NotImplementedError(
                f"Database.query executes SQL and Mongo dialects; got plan "
                f"{plan!r}"
            )
        op = plan.get("op")
        table = plan["table"]
        if op == "select":
            return self._query_select(plan, where_to_query)
        if op == "insert":
            data = plan["data"]
            if "_id" not in data:
                raise ValueError("INSERT requires an \"_id\" field for the key")
            key = _bytes(str(data["_id"]))
            self.put(table, key, json.dumps(data))
            return key
        if op == "delete":
            matcher = where_to_query(plan.get("where"))
            affected = 0
            for key, _document in self.find_query(table, matcher):
                self.delete(table, key)
                affected += 1
            return affected
        if op == "update":
            matcher = where_to_query(plan.get("where"))
            updates = plan["data"]
            affected = 0
            for key, document in self.find_query(table, matcher):
                document.update(updates)
                self.put(table, key, json.dumps(document))
                affected += 1
            return affected
        raise NotImplementedError(f"SQL op {op!r} is not executed by query()")

    def _query_mongo(self, plan: dict):
        """Execute a db.<collection>.<op>(args) plan. find -> list[dict];
        findOne -> dict|None; insert/insertOne -> key (bytes, needs _id);
        deleteOne/deleteMany -> int; count -> int."""
        import json

        collection = plan["collection"]
        op = plan["op"]
        args = plan.get("args") or {}
        if op == "find":
            return [document for _key, document in self.find_query(collection, args)]
        if op == "findOne":
            for _key, document in self.find_query(collection, args):
                return document
            return None
        if op in ("insert", "insertOne"):
            if not isinstance(args, dict) or "_id" not in args:
                raise ValueError("insert requires an \"_id\" field for the key")
            key = _bytes(str(args["_id"]))
            self.put(collection, key, json.dumps(args))
            return key
        if op in ("deleteOne", "deleteMany"):
            affected = 0
            for key, _document in self.find_query(collection, args):
                self.delete(collection, key)
                affected += 1
                if op == "deleteOne":
                    break
            return affected
        if op == "count":
            return len(self.scan(collection))
        raise NotImplementedError(f"mongo op {op!r} is not executed by query()")

    def _query_select(self, plan: dict, where_to_query) -> list[dict]:
        matcher = where_to_query(plan.get("where"))
        rows = [document for _key, document in self.find_query(plan["table"], matcher)]
        order_by = plan.get("order_by")
        if order_by:
            field = order_by
            reverse = False
            lowered = order_by.lower()
            if lowered.endswith(" desc"):
                field, reverse = order_by[:-5].strip(), True
            elif lowered.endswith(" asc"):
                field = order_by[:-4].strip()

            def sort_key(document, _field=field):
                value = document.get(_field)
                if value is None:
                    return (1, "", 0)
                return (0, type(value).__name__, value)

            rows.sort(key=sort_key, reverse=reverse)
        offset = plan.get("offset") or 0
        if offset:
            rows = rows[offset:]
        limit = plan.get("limit")
        if limit:
            rows = rows[:limit]
        return rows

    def verify(self) -> dict[str, int]:
        result = _VerifyResult()
        _raise_status(_lib.sdb_database_verify(
            self._require_open(), ctypes.byref(result)
        ))
        return {
            name: int(getattr(result, name))
            for name, _ctype in _VerifyResult._fields_
            if name not in {"reserved", "reserved_alignment"}
        }

    def backup(self, destination, *, replace: bool = False) -> int:
        result = _BackupResult()
        _raise_status(_lib.sdb_database_backup(
            self._require_open(), _path(destination), replace,
            ctypes.byref(result),
        ))
        return int(result.byte_count)

    def _rewrite(
        self,
        *,
        target_version: int | None,
        password: bytes | str | None,
        page_size: int,
        kdf_iterations: int,
    ) -> dict[str, int]:
        options, keepalive = _options(password, page_size, kdf_iterations)
        result = _CompactResult()
        if target_version is None:
            status = _lib.sdb_database_compact(
                self._require_open(), ctypes.byref(options),
                ctypes.byref(result),
            )
        else:
            status = _lib.sdb_database_migrate(
                self._require_open(), target_version, ctypes.byref(options),
                ctypes.byref(result),
            )
        del keepalive
        _raise_status(status)
        return {
            name: int(getattr(result, name))
            for name, _ctype in _CompactResult._fields_
            if name != "reserved"
        }

    def compact(
        self,
        *,
        password: bytes | str | None = None,
        page_size: int = 4096,
        kdf_iterations: int = 600000,
    ) -> dict[str, int]:
        return self._rewrite(
            target_version=None,
            password=password,
            page_size=page_size,
            kdf_iterations=kdf_iterations,
        )

    def migrate(
        self,
        target_version: int,
        *,
        password: bytes | str | None = None,
        page_size: int = 4096,
        kdf_iterations: int = 600000,
    ) -> dict[str, int]:
        return self._rewrite(
            target_version=target_version,
            password=password,
            page_size=page_size,
            kdf_iterations=kdf_iterations,
        )

class Transaction:

    def __init__(
        self, database: Database, handle: _transaction_p
    ) -> None:
        self._database = database
        self._handle = handle

    @classmethod
    def _begin(cls, database: Database) -> "Transaction":
        handle = _transaction_p()
        _raise_status(_lib.sdb_transaction_begin(
            database._require_open(), ctypes.byref(handle)
        ))
        return cls(database, handle)

    def _require_active(self) -> _transaction_p:
        if not self._handle:
            raise RuntimeError("transaction is closed")
        return self._handle

    def _finish(self, function) -> None:
        handle = self._require_active()
        status = function(handle)
        close_status = _lib.sdb_transaction_close(handle)
        if close_status == 0:
            self._handle = _transaction_p()
        if status != 0:
            _raise_status(status)
        _raise_status(close_status)

    def commit(self) -> None:
        self._finish(_lib.sdb_transaction_commit)

    def rollback(self) -> None:
        self._finish(_lib.sdb_transaction_rollback)

    def close(self) -> None:
        if self._handle:
            self.rollback()

    def __enter__(self) -> "Transaction":
        self._require_active()
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if exc_type is None:
            self.commit()
        else:
            self.rollback()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _put(self, kind: str, namespace, key, value) -> None:
        namespace_bytes, key_bytes, value_bytes = map(
            _bytes, (namespace, key, value)
        )
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        value_store, value_ptr = _buffer(value_bytes)
        status = getattr(_lib, f"sdb_transaction_{kind}_put")(
            self._require_active(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
            value_ptr, len(value_bytes),
        )
        del ns_store, key_store, value_store
        _raise_status(status)

    def _get(self, kind: str, namespace, key) -> bytes:
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        function = getattr(_lib, f"sdb_transaction_{kind}_get")
        result = _dynamic_get(
            function,
            self._require_active(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
        )
        del ns_store, key_store
        return result

    def _delete(self, kind: str, namespace, key) -> None:
        namespace_bytes, key_bytes = map(_bytes, (namespace, key))
        ns_store, ns_ptr = _buffer(namespace_bytes)
        key_store, key_ptr = _buffer(key_bytes)
        status = getattr(_lib, f"sdb_transaction_{kind}_delete")(
            self._require_active(),
            ns_ptr, len(namespace_bytes), key_ptr, len(key_bytes),
        )
        del ns_store, key_store
        _raise_status(status)

    def put(self, namespace, key, value) -> None:
        self._put("kv", namespace, key, value)

    def get(self, namespace, key) -> bytes:
        return self._get("kv", namespace, key)

    def delete(self, namespace, key) -> None:
        self._delete("kv", namespace, key)

    def put_blob(self, namespace, key, value) -> None:
        self._put("blob", namespace, key, value)

    def get_blob(self, namespace, key) -> bytes:
        return self._get("blob", namespace, key)

    def delete_blob(self, namespace, key) -> None:
        self._delete("blob", namespace, key)

    def create_index(
        self, collection, name, *, unique: bool = False
    ) -> None:
        collection_bytes, name_bytes = map(_bytes, (collection, name))
        collection_store, collection_ptr = _buffer(collection_bytes)
        name_store, name_ptr = _buffer(name_bytes)
        status = _lib.sdb_transaction_index_create(
            self._require_active(),
            collection_ptr, len(collection_bytes),
            name_ptr, len(name_bytes), unique,
        )
        del collection_store, name_store
        _raise_status(status)

    def put_document(
        self,
        collection,
        document_id,
        document,
        *,
        terms: Sequence[tuple[bytes | str, bytes | str]] = (),
    ) -> None:
        collection_bytes, id_bytes, document_bytes = map(
            _bytes, (collection, document_id, document)
        )
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        document_store, document_ptr = _buffer(document_bytes)
        term_array = (_IndexTerm * len(terms))()
        term_keepalive = []
        for index, (name, value) in enumerate(terms):
            name_bytes, value_bytes = map(_bytes, (name, value))
            name_store, name_ptr = _buffer(name_bytes)
            value_store, value_ptr = _buffer(value_bytes)
            term_keepalive.extend((name_store, value_store))
            term_array[index] = _IndexTerm(
                name_ptr, len(name_bytes), value_ptr, len(value_bytes)
            )
        status = _lib.sdb_transaction_document_put(
            self._require_active(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
            document_ptr, len(document_bytes),
            term_array if terms else None, len(terms),
        )
        del collection_store, id_store, document_store, term_keepalive
        _raise_status(status)

    def get_document(self, collection, document_id) -> bytes:
        collection_bytes, id_bytes = map(_bytes, (collection, document_id))
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        result = _dynamic_get(
            _lib.sdb_transaction_document_get,
            self._require_active(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
        )
        del collection_store, id_store
        return result

    def delete_document(self, collection, document_id) -> None:
        collection_bytes, id_bytes = map(_bytes, (collection, document_id))
        collection_store, collection_ptr = _buffer(collection_bytes)
        id_store, id_ptr = _buffer(id_bytes)
        status = _lib.sdb_transaction_document_delete(
            self._require_active(),
            collection_ptr, len(collection_bytes), id_ptr, len(id_bytes),
        )
        del collection_store, id_store
        _raise_status(status)

def migrate_file(
    source,
    destination,
    *,
    source_password: bytes | str | None = None,
    target_password: bytes | str | None = None,
    page_size: int = 4096,
    kdf_iterations: int = 600000,
) -> dict[str, int]:
    """Upgrade a V1 database file to the current V2 format, writing a NEW file
    at destination (source is left untouched). Only a V1 source is accepted.
    source_password decrypts the source; target_password (default same behaviour
    as source=None -> plaintext) encrypts the destination and may differ. Returns
    the byte/entry counters. The rewrite does not reclaim stale index rows; run
    Database.compact on the destination if needed."""
    source_options, source_store = _options(
        source_password, page_size, kdf_iterations
    )
    target_options, target_store = _options(
        target_password, page_size, kdf_iterations
    )
    result = _CompactResult()
    status = _lib.sdb_database_migrate_file(
        _path(source), _path(destination),
        ctypes.byref(source_options), ctypes.byref(target_options),
        ctypes.byref(result),
    )
    del source_store, target_store
    _raise_status(status)
    return {
        name: int(getattr(result, name))
        for name, _ctype in _CompactResult._fields_
        if name != "reserved"
    }


__all__ = [
    "ABI_VERSION",
    "SDB_CACHE_AUTO",
    "SDB_SYNCHRONOUS_FULL",
    "SDB_SYNCHRONOUS_NORMAL",
    "AuthenticationError",
    "BusyError",
    "ConflictError",
    "CorruptionError",
    "Database",
    "Error",
    "NotFoundError",
    "Transaction",
    "abi_version",
    "migrate_file",
    "version",
]
