# -----------------------------------------------------------------------
#
# pgrac-gdb-helpers.py
#    Python gdb extensions for pgrac cluster diagnostics (Stage 0.29).
#
#    Usage (in a gdb session attached to a running postgres backend):
#        (gdb) source /path/to/pgrac-gdb-helpers.py
#        (gdb) cluster-info
#        (gdb) cluster-injections
#        (gdb) cluster-pgstat
#
#    Provides three commands and three pretty-printers (ClusterShmemCtl
#    / ClusterPgstatCounter / ClusterInjectPoint) for inspecting the
#    cluster subsystem state at gdb level.  Equivalent to the SQL view
#    pg_cluster_state but works on a frozen / dumped backend (e.g. when
#    attached to a core file or a stuck process).
#
# IDENTIFICATION
#    src/tools/pgrac-gdb-helpers.py
#
# Author: SqlRush <sqlrush@gmail.com>
#
# Portions Copyright (c) 2026, pgrac contributors
#
# NOTES
#    Read-only.  Never modifies backend state.  Never calls
#    gdb.execute('continue') or signal-injection commands.
#
#    Compatibility:
#        Python 3.6+, gdb 9+ (Linux), gdb 13+ via Homebrew (macOS).
#        LLDB is NOT supported (gdb python API is incompatible with LLDB
#        scripting bridge).  See pgrac/docs/cluster-debug-design.md §6
#        FAQ for the macOS gdb code-signing setup.
#
#    Adding a new command at Stage 1+:  copy one of the three Command
#    classes below as a template, register it via ".register(self)" at
#    file load time, and document it in pgrac/docs/cluster-debug-design.md
#    §4.
#
# -----------------------------------------------------------------------

import gdb


# Marker so re-sourcing the file is detectable.
PGRAC_GDB_HELPERS_VERSION = "0.29"


# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

def _try_eval(expr):
    """Evaluate a gdb expression; return the gdb.Value or None on error.

    Used to defensively probe symbols that may not exist in a particular
    backend (e.g. cluster_inject is built but cluster_pgstat is not, or
    a --disable-cluster build has neither).
    """
    try:
        return gdb.parse_and_eval(expr)
    except gdb.error:
        return None


def _atomic_u32_read(value):
    """Read a pg_atomic_uint32 .value field.

    The exact field layout depends on the PG atomics backend; on every
    platform pgrac targets there is a `value` field of u32 width.  This
    helper isolates that assumption so future PG upstream changes only
    require one edit.
    """
    try:
        return int(value['value'])
    except gdb.error:
        return None


def _atomic_u64_read(value):
    try:
        return int(value['value'])
    except gdb.error:
        return None


def _format_kv(rows, max_key_width=None):
    """Format a list of (key, value) pairs as right-padded text rows."""
    if not rows:
        return "(no rows)"
    width = max_key_width if max_key_width is not None else max(
        len(k) for k, _ in rows)
    out_lines = []
    for k, v in rows:
        out_lines.append("  {:<{w}}  {}".format(k, v, w=width))
    return "\n".join(out_lines)


def _fault_type_name(t):
    """Decode ClusterInjectFaultType enum int -> string."""
    return {
        0: "none",
        1: "error",
        2: "warning",
        3: "sleep",
        4: "crash",
        5: "skip",
    }.get(t, "unknown")


# -----------------------------------------------------------------------
# Commands
# -----------------------------------------------------------------------

class ClusterInfoCommand(gdb.Command):
    """Show pgrac cluster top-level state.

    Reports the ClusterShmem control block (magic / version /
    node_id_at_init / created_at), the cluster.* GUC current values
    (cluster_node_id / cluster_interconnect_tier / cluster_config_file
    / cluster_injection_points), the active interconnect tier vtable
    name, and the cluster_phase lifecycle string.

    Read-only; safe to invoke on a paused backend or core file.
    """

    def __init__(self):
        super().__init__("cluster-info", gdb.COMMAND_USER, gdb.COMPLETE_NONE)

    def invoke(self, arg, from_tty):
        rows = []

        shmem = _try_eval("ClusterShmem")
        if shmem and int(shmem) != 0:
            ctl = shmem.dereference()
            rows.append(("shmem.magic", "0x{:08X}".format(int(ctl['magic']))))
            rows.append(("shmem.version_packed",
                         "0x{:08X}".format(int(ctl['version_packed']))))
            rows.append(("shmem.node_id_at_init",
                         str(int(ctl['node_id_at_init']))))
            rows.append(("shmem.created_at", str(ctl['created_at'])))
        else:
            rows.append(("shmem", "(NULL: ClusterShmem not initialised)"))

        node_id = _try_eval("cluster_node_id")
        rows.append(("guc.cluster.node_id",
                     str(int(node_id)) if node_id is not None else "(unavailable)"))
        tier = _try_eval("cluster_interconnect_tier")
        rows.append(("guc.cluster.interconnect_tier",
                     str(int(tier)) if tier is not None else "(unavailable)"))
        cfg = _try_eval("cluster_config_file")
        rows.append(("guc.cluster.config_file",
                     str(cfg.string()) if cfg and int(cfg) != 0 else "(empty)"))
        ip = _try_eval("cluster_injection_points")
        rows.append(("guc.cluster.injection_points",
                     str(ip.string()) if ip and int(ip) != 0 else "(empty)"))

        active = _try_eval("ClusterICOps_Active")
        if active and int(active) != 0:
            tier_name = active.dereference()['tier_name']
            rows.append(("ic.active_tier_name",
                         str(tier_name.string()) if int(tier_name) != 0 else "(null)"))
        else:
            rows.append(("ic.active_tier_name", "(null)"))

        phase = _try_eval("cluster_phase")
        rows.append(("phase.cluster_phase",
                     str(phase.string()) if phase and int(phase) != 0 else "(unset)"))

        print("=== pgrac cluster-info ===")
        print(_format_kv(rows))


class ClusterInjectionsCommand(gdb.Command):
    """List pgrac cluster injection points with armed state + hits.

    Iterates the cluster_injection_points[] static array in
    cluster_inject.c.  Output is one row per injection point:
      <name>  <fault_type>  <hits>
    """

    def __init__(self):
        super().__init__("cluster-injections",
                         gdb.COMMAND_USER, gdb.COMPLETE_NONE)

    def invoke(self, arg, from_tty):
        # cluster_injection_points is a static array; we can only see
        # its size constant CLUSTER_INJECTION_COUNT through the
        # accessor, but the elements live as a linear array we can
        # index directly.  We probe by name lookup of the symbol.
        armed_count = _try_eval("cluster_injection_armed_count")
        if armed_count is None:
            print("cluster_injection_armed_count not available "
                  "(disable-cluster build?)")
            return

        print("=== pgrac cluster-injections ===")
        print("armed_count: {}".format(int(armed_count)))
        print()

        # Iterate via the public accessor names (visible because
        # cluster_inject_get_count / _get_state_at are extern in
        # enable-cluster builds).
        count_val = _try_eval("cluster_injection_get_count()")
        if count_val is None:
            print("cluster_injection_get_count() not available; cannot "
                  "iterate registry.")
            return

        n = int(count_val)
        rows = [("name", "fault_type", "hits")]
        # We can't easily call cluster_injection_get_state_at from gdb
        # without setting up output parameters.  Fall back to direct
        # array access via the `cluster_injection_points` symbol.
        registry = _try_eval("cluster_injection_points")
        if registry is None:
            print("cluster_injection_points symbol not visible "
                  "(disable-cluster build or stripped binary?)")
            return

        for i in range(n):
            entry = registry[i]
            name = entry['name']
            name_str = str(name.string()) if int(name) != 0 else "(null)"
            armed_type = _atomic_u32_read(entry['armed_type'])
            hits = _atomic_u64_read(entry['hits'])
            rows.append((name_str,
                         _fault_type_name(armed_type) if armed_type is not None else "(unavailable)",
                         str(hits) if hits is not None else "(unavailable)"))

        # Print as aligned table.
        widths = [max(len(r[c]) for r in rows) for c in range(3)]
        for r in rows:
            print("  {:<{w0}}  {:<{w1}}  {:<{w2}}".format(
                r[0], r[1], r[2],
                w0=widths[0], w1=widths[1], w2=widths[2]))


class ClusterPgstatCommand(gdb.Command):
    """List pgrac cluster pgstat atomic counters.

    Iterates the cluster_pgstat_counters[] static array in
    cluster_pgstat.c.  Output is one row per counter:
      <name>  <value>
    """

    def __init__(self):
        super().__init__("cluster-pgstat",
                         gdb.COMMAND_USER, gdb.COMPLETE_NONE)

    def invoke(self, arg, from_tty):
        count_val = _try_eval("cluster_pgstat_get_count()")
        if count_val is None:
            print("cluster_pgstat_get_count() not available "
                  "(disable-cluster build?)")
            return

        n = int(count_val)
        registry = _try_eval("cluster_pgstat_counters")
        if registry is None:
            print("cluster_pgstat_counters symbol not visible.")
            return

        print("=== pgrac cluster-pgstat ===")
        rows = [("name", "value")]
        for i in range(n):
            entry = registry[i]
            name = entry['name']
            name_str = str(name.string()) if int(name) != 0 else "(null)"
            value = _atomic_u64_read(entry['value'])
            rows.append((name_str,
                         str(value) if value is not None else "(unavailable)"))
        widths = [max(len(r[c]) for r in rows) for c in range(2)]
        for r in rows:
            print("  {:<{w0}}  {:<{w1}}".format(
                r[0], r[1], w0=widths[0], w1=widths[1]))


# -----------------------------------------------------------------------
# Pretty-printers
# -----------------------------------------------------------------------

class ClusterShmemCtlPrinter:
    """Pretty-printer for ClusterShmemCtl."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return ("ClusterShmemCtl(magic=0x{:08X}, version=0x{:08X}, "
                "node_id_at_init={}, created_at={})".format(
                    int(self.val['magic']),
                    int(self.val['version_packed']),
                    int(self.val['node_id_at_init']),
                    self.val['created_at']))


class ClusterPgstatCounterPrinter:
    """Pretty-printer for ClusterPgstatCounter."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        name = self.val['name']
        name_str = str(name.string()) if int(name) != 0 else "(null)"
        value = _atomic_u64_read(self.val['value'])
        return 'ClusterPgstatCounter(name="{}", value={})'.format(
            name_str, value if value is not None else "?")


class ClusterInjectPointPrinter:
    """Pretty-printer for ClusterInjectPoint."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        name = self.val['name']
        name_str = str(name.string()) if int(name) != 0 else "(null)"
        armed = _atomic_u32_read(self.val['armed_type'])
        hits = _atomic_u64_read(self.val['hits'])
        return ('ClusterInjectPoint(name="{}", armed_type={}, hits={})'
                .format(name_str,
                        _fault_type_name(armed) if armed is not None else "?",
                        hits if hits is not None else "?"))


def _build_pretty_printer():
    import gdb.printing
    pp = gdb.printing.RegexpCollectionPrettyPrinter("pgrac")
    pp.add_printer('ClusterShmemCtl',
                   '^ClusterShmemCtl$', ClusterShmemCtlPrinter)
    pp.add_printer('ClusterPgstatCounter',
                   '^ClusterPgstatCounter$', ClusterPgstatCounterPrinter)
    pp.add_printer('ClusterInjectPoint',
                   '^ClusterInjectPoint$', ClusterInjectPointPrinter)
    return pp


# -----------------------------------------------------------------------
# File-load registration
# -----------------------------------------------------------------------

# Register commands once per source.  Re-source-ing the file is
# tolerated (gdb auto-replaces existing command objects).
ClusterInfoCommand()
ClusterInjectionsCommand()
ClusterPgstatCommand()

# Register pretty-printers on the current objfile (typically the
# postgres binary the user just attached).  replace=True so re-source
# does not stack duplicates.
try:
    import gdb.printing
    gdb.printing.register_pretty_printer(
        gdb.current_objfile(),
        _build_pretty_printer(),
        replace=True)
except Exception as exc:  # pragma: no cover (printing setup, gdb-side)
    print("pgrac-gdb-helpers: pretty-printer registration skipped: {}"
          .format(exc))


print("pgrac-gdb-helpers v{} loaded.  Commands: cluster-info / "
      "cluster-injections / cluster-pgstat.".format(PGRAC_GDB_HELPERS_VERSION))
