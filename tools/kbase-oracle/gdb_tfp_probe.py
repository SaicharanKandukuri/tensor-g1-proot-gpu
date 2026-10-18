import gdb
import struct
import zlib


state = {
    "pending": None,
    "seen": set(),
    "completed": 0,
    "breakpoints": [],
    "map_hits": 0,
}


def reg(name):
    return int(gdb.parse_and_eval("$" + name))


def read(addr, size):
    return gdb.selected_inferior().read_memory(addr, size).tobytes()


def u32(addr):
    return struct.unpack("<I", read(addr, 4))[0]


def s32(addr):
    return struct.unpack("<i", read(addr, 4))[0]


def u64(addr):
    return struct.unpack("<Q", read(addr, 8))[0]


def row_crc(addr, stride, row_bytes, height):
    span = read(addr, stride * (height - 1) + row_bytes)
    crc = 0
    for y in range(height):
        start = y * stride
        crc = zlib.crc32(span[start:start + row_bytes], crc)
    return crc


def samples(addr, stride, height):
    rows = sorted(set([0, min(30, height - 1), min(60, height - 1), height - 1]))
    return ",".join(f"{row}:{read(addr + row * stride, 4).hex()}" for row in rows)


class MapBreakpoint(gdb.Breakpoint):
    def stop(self):
        sp = reg("sp")
        res = reg("x24")
        mapped = reg("x22")
        transfer = u64(sp + 32)
        x = s32(sp + 16)
        y = s32(sp + 20)
        width = s32(sp + 24)
        height = s32(sp + 28)
        state["map_hits"] += 1
        if state["map_hits"] <= 10:
            gdb.write(
                f"TFP_RAW_HIT count={state['map_hits']} res=0x{res:x} "
                f"extent={width}x{height} map=0x{mapped:x} "
                f"transfer=0x{transfer:x}\n"
            )
        if (width, height) not in ((500, 396), (640, 516)) or res in state["seen"]:
            state["pending"] = None
            return False
        state["seen"].add(res)
        resource_width = u32(res + 64)
        resource_height = u32(res + 68)
        format_target_level = u32(res + 76)
        usage_samples = u32(res + 80)
        bind = u32(res + 84)
        flags = u32(res + 88)
        stride = u32(transfer + 32)
        state["pending"] = {
            "res": res,
            "map": mapped,
            "transfer": transfer,
            "width": width,
            "height": height,
            "stride": stride,
        }
        gdb.write(
            f"TFP_MAP res=0x{res:x} map=0x{mapped:x} transfer=0x{transfer:x} "
            f"xy={x},{y} extent={width}x{height} "
            f"resource_extent={resource_width}x{resource_height} "
            f"format_target_level=0x{format_target_level:08x} "
            f"usage_samples=0x{usage_samples:08x} bind=0x{bind:08x} "
            f"flags=0x{flags:08x} stride={stride}\n"
        )
        return False


class TightBreakpoint(gdb.Breakpoint):
    def stop(self):
        item = state["pending"]
        if item is None:
            return False
        row_bytes = item["width"] * 4
        item["tight_crc"] = row_crc(item["map"], row_bytes, row_bytes,
                                    item["height"])
        item["tight_samples"] = samples(item["map"], row_bytes,
                                        item["height"])
        return False


class ExpandedBreakpoint(gdb.Breakpoint):
    def stop(self):
        item = state["pending"]
        if item is None:
            return False
        row_bytes = item["width"] * 4
        expanded_crc = row_crc(item["map"], item["stride"], row_bytes,
                               item["height"])
        expanded_samples = samples(item["map"], item["stride"],
                                   item["height"])
        gdb.write(
            f"TFP_COPY res=0x{item['res']:x} "
            f"tight_crc={item['tight_crc']:08x} expanded_crc={expanded_crc:08x} "
            f"crc_match={int(item['tight_crc'] == expanded_crc)} "
            f"tight_samples={item['tight_samples']} "
            f"expanded_samples={expanded_samples} "
            f"samples_match={int(item['tight_samples'] == expanded_samples)}\n"
        )
        state["completed"] += 1
        state["pending"] = None
        return False


class EntryBreakpoint(gdb.Breakpoint):
    def stop(self):
        base = int(gdb.parse_and_eval("(void *)drisw_update_tex_buffer"))
        addresses = (base + 212, base + 404, base + 488)
        state["breakpoints"] = [
            MapBreakpoint(f"*0x{addresses[0]:x}"),
            TightBreakpoint(f"*0x{addresses[1]:x}"),
            ExpandedBreakpoint(f"*0x{addresses[2]:x}"),
        ]
        self.enabled = False
        gdb.write(
            f"TFP_PROBE_BASE address=0x{base:x} "
            f"map=0x{addresses[0]:x} tight=0x{addresses[1]:x} "
            f"expanded=0x{addresses[2]:x}\n"
        )
        return False


state["entry"] = EntryBreakpoint("drisw_update_tex_buffer", internal=True)
