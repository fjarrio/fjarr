#!/usr/bin/env python3
"""Structural diff of two flattened device trees, ignoring phandle numbering.

    dtbdiff.py A.dtb B.dtb               what B changes against A; exit status 1 if anything
                                         does, 0 if nothing, 2 if a file cannot be read
    dtbdiff.py --dump A.dtb /path ...    the nodes under each path, with their properties

spec: docs/32-fleet-ota.md#jetson-test-image (the board package's carrier device tree)

A file may carry bytes after the tree, as a partition image does: only the header's totalsize
is read. `phandle` properties are never compared, and a cell that refers to a node compares
and prints as that node's path, so two builds of one source that number their phandles
differently compare equal. Which cells are references is read from the property's binding
(`phys`, `clocks`, GPIO specifiers and the like, sized by the provider's `#...-cells`). A
property without a known binding is equal when its raw cells are, or when every differing cell
is a phandle to the same node in both trees; it prints as raw cells, and the second kind is
counted on stderr. Such a property could hide a reference moved to a node that happens to
carry the old number, which two builds of one source do not produce.

Reads data only (no libfdt): run it as `python3 -I dtbdiff.py ...`.
"""
import re
import struct
import sys

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9

# A property naming providers, with the provider's property that sizes each specifier
# (None: a bare phandle, no argument cells).
SPECIFIER = {
    'phys': '#phy-cells',
    'clocks': '#clock-cells',
    'assigned-clocks': '#clock-cells',
    'assigned-clock-parents': '#clock-cells',
    'resets': '#reset-cells',
    'power-domains': '#power-domain-cells',
    'pwms': '#pwm-cells',
    'dmas': '#dma-cells',
    'iommus': '#iommu-cells',
    'mboxes': '#mbox-cells',
    'interconnects': '#interconnect-cells',
    'thermal-sensors': '#thermal-sensor-cells',
    'sound-dai': '#sound-dai-cells',
    'io-channels': '#io-channel-cells',
    'interrupts-extended': '#interrupt-cells',
    'hwlocks': '#hwlock-cells',
    'mux-controls': '#mux-control-cells',
    'cooling-device': '#cooling-cells',
    'nvmem-cells': None,
    'memory-region': None,
    'interrupt-parent': None,
    'remote-endpoint': None,
    'operating-points-v2': None,
    'next-level-cache': None,
    'cpu': None,
    'cpus': None,
    'cpu-idle-states': None,
}
# Plain numbers, never references: compared as raw cells.
NUMBERS = {'reg', 'ranges', 'dma-ranges', 'interrupts', 'bus-range', 'clock-frequency',
           'linux,code', 'linux,input-type', 'nvidia,usb2-companion', 'num-lanes'}


def rule(node, name):
    """The specifier rule for a property, or False when it has none."""
    if name in SPECIFIER:
        return SPECIFIER[name]
    if name.endswith('-supply') or re.fullmatch(r'pinctrl-\d+', name):
        return None
    if name in ('gpio', 'gpios') or name.endswith(('-gpio', '-gpios')):
        # A GPIO hog's `gpios` holds pin and flags only, no controller.
        return False if 'gpio-hog' in node else '#gpio-cells'
    return False


class Bad(Exception):
    """An input that is not a device tree this tool can read."""


def is_strings(v):
    if not v or v[-1:] != b'\0':
        return False
    parts = v[:-1].split(b'\0')
    return all(p and all(32 <= c < 127 for c in p) for p in parts)


def cells(v):
    return struct.unpack('>%dI' % (len(v) // 4), v)


class Tree:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < 40:
            raise Bad(f'{path}: too short for a device tree')
        (magic, total, off_struct, off_strings, off_rsv, version, _last, self.boot_cpuid,
         size_strings, size_struct) = struct.unpack_from('>10I', data)
        if magic != FDT_MAGIC:
            raise Bad(f'{path}: not a flattened device tree')
        if version < 17 or total > len(data):
            raise Bad(f'{path}: version {version}, totalsize {total} of {len(data)} bytes')
        self.path, self.size, self.trailing = path, total, len(data) - total
        data = data[:total]
        self.memreserve, pos = [], off_rsv
        while True:
            addr, size = struct.unpack_from('>QQ', data, pos)
            pos += 16
            if addr == 0 and size == 0:
                break
            self.memreserve.append((addr, size))
        strings = data[off_strings:off_strings + size_strings]
        self.nodes, stack, pos = {}, [], off_struct
        while pos < off_struct + size_struct:
            (tok,) = struct.unpack_from('>I', data, pos)
            pos += 4
            if tok == BEGIN_NODE:
                end = data.index(b'\0', pos)
                stack.append(data[pos:end].decode('ascii'))
                pos = (end + 4) & ~3
                here = '/' + '/'.join(stack[1:])
                if here in self.nodes:
                    raise Bad(f'{path}: node {here} appears twice')
                self.nodes[here] = {}
            elif tok == END_NODE:
                stack.pop()
            elif tok == PROP:
                length, nameoff = struct.unpack_from('>II', data, pos)
                pos += 8
                name = strings[nameoff:strings.index(b'\0', nameoff)].decode('ascii')
                self.nodes['/' + '/'.join(stack[1:])][name] = data[pos:pos + length]
                pos = (pos + length + 3) & ~3
            elif tok == NOP:
                continue
            elif tok == END:
                break
            else:
                raise Bad(f'{path}: bad structure token {tok:#x} at {pos - 4:#x}')
        self.phandles = {}
        for here, props in self.nodes.items():
            for k in ('phandle', 'linux,phandle'):
                v = props.get(k)
                if v is not None and len(v) == 4:
                    self.phandles[cells(v)[0]] = here

    def specifiers(self, node, name, v):
        """The property as (('&', path), args...) per its binding, or None."""
        r = rule(self.nodes[node], name)
        if r is False or not v or len(v) % 4:
            return None
        c, out, i = cells(v), [], 0
        while i < len(c):
            if c[i] == 0:  # an empty entry in a list, as `cs-gpios = <0>, <&gpio ...>`
                out.append(0)
                i += 1
                continue
            target = self.phandles.get(c[i])
            if target is None:
                return None
            n = 0
            if r is not None:
                size = self.nodes[target].get(r)
                if size is None or len(size) != 4:
                    return None
                n = cells(size)[0]
            if i + 1 + n > len(c):
                return None
            out.append(('&', target))
            out.extend(c[i + 1:i + 1 + n])
            i += 1 + n
        return tuple(out)

    def loose(self, v):
        return tuple(('&', self.phandles[x]) if x in self.phandles else x for x in cells(v))

    def show(self, node, name, v):
        if not v:
            return '(empty)'
        if is_strings(v):
            return ', '.join('"%s"' % s.decode() for s in v[:-1].split(b'\0'))
        spec = self.specifiers(node, name, v)
        if spec is not None:
            return '<%s>' % ' '.join('&{%s}' % x[1] if isinstance(x, tuple) else '%#x' % x
                                     for x in spec)
        if len(v) % 4 == 0:
            return '<%s>' % ' '.join('%#x' % x for x in cells(v))
        return '[%s]' % v.hex(' ')


def compare(a, b, node, name):
    """(equal, equal_only_by_guess) for one property present in both trees."""
    va, vb = a.nodes[node][name], b.nodes[node][name]
    if is_strings(va) and is_strings(vb) or name in NUMBERS or name.startswith('#'):
        return va == vb, False
    sa, sb = a.specifiers(node, name, va), b.specifiers(node, name, vb)
    if sa is not None and sb is not None:
        return sa == sb, False
    # No binding: equal raw cells are equal (a number that happens to be a phandle in one
    # tree is still that number), and differing cells are equal only if each is a phandle to
    # the same node in both trees.
    if va == vb:
        return True, False
    if len(va) % 4 or len(vb) % 4 or not va or not vb:
        return False, False
    same = a.loose(va) == b.loose(vb)
    return same, same


SKIP = ('phandle', 'linux,phandle')


def diff(a, b):
    out, guessed = [], 0
    if a.memreserve != b.memreserve:
        out.append('~ /memreserve/ %s -> %s' % (a.memreserve, b.memreserve))
    if a.boot_cpuid != b.boot_cpuid:
        out.append('~ boot_cpuid %#x -> %#x' % (a.boot_cpuid, b.boot_cpuid))
    for node in sorted(set(a.nodes) - set(b.nodes)):
        out.append('- node %s' % node)
    for node in sorted(set(b.nodes) - set(a.nodes)):
        out.append('+ node %s' % node)
        for name in sorted(b.nodes[node]):
            if name not in SKIP:
                out.append('    %s = %s' % (name, b.show(node, name, b.nodes[node][name])))
    for node in sorted(set(a.nodes) & set(b.nodes)):
        pa, pb = a.nodes[node], b.nodes[node]
        for name in sorted(set(pa) | set(pb)):
            if name in SKIP:
                continue
            if name not in pb:
                out.append('- %s: %s = %s' % (node, name, a.show(node, name, pa[name])))
            elif name not in pa:
                out.append('+ %s: %s = %s' % (node, name, b.show(node, name, pb[name])))
            else:
                same, guess = compare(a, b, node, name)
                guessed += guess
                if not same:
                    out.append('~ %s: %s = %s -> %s' % (node, name, a.show(node, name, pa[name]),
                                                         b.show(node, name, pb[name])))
    return out, guessed


def main(argv):
    if len(argv) >= 3 and argv[0] == '--dump':
        t = Tree(argv[1])
        for node in sorted(t.nodes):
            if any(node == p or node.startswith(p.rstrip('/') + '/') for p in argv[2:]):
                print(node)
                for name in sorted(t.nodes[node]):
                    if name not in SKIP:
                        print('    %s = %s' % (name, t.show(node, name, t.nodes[node][name])))
        return 0
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    a, b = Tree(argv[0]), Tree(argv[1])
    out, guessed = diff(a, b)
    for line in out:
        print(line)
    print('dtbdiff: %d nodes / %d nodes, %d differences; %d properties equal by phandle '
          'path only; trailing bytes %d / %d' % (len(a.nodes), len(b.nodes), len(out), guessed,
                                                  a.trailing, b.trailing), file=sys.stderr)
    return 1 if out else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (Bad, OSError, ValueError, IndexError, KeyError, struct.error) as e:
        print('dtbdiff: %s' % e, file=sys.stderr)
        sys.exit(2)
