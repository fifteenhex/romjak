# romjak

For jacking them roms.

Takes a binary and splits it across a set of ROM images ready for burning,
and puts a set of images back together again. It handles the two things
that make this fiddly by hand: ROMs sitting side by side on a data bus
wider than one chip, and banks stacked up the address space.

## Building

```
meson setup build
meson compile -C build
meson test -C build
```

argtable3 comes from a wrap, so there is nothing to install first.

## Splitting

```
romjak split --numroms=<n> --romsize=<n> [--romwidth=<n>] [--rombanks=<n>]
             [--paduptosize=<n>] [--pad=<byte>] [-n] <file> [<basename>]
```

Sizes take `0x` hex and `KB`/`MB` suffixes, so `--romsize=32KB` and
`--romsize=0x8000` are the same thing.

`--dry-run`/`-n` draws the layout and stops before writing anything, which
is worth doing before you commit a set of parts:

```
$ romjak split -n --numroms=4 --rombanks=2 --romsize=32KB --paduptosize=32KB game.bin game
Splitting game.bin (700 bytes) over 4 ROMs of 32768 bytes, 2 bank(s), 8-bit each:

            byte +0      byte +1
         +------------+------------+
 bank 0  |  game.0.0  |  game.0.1  |  0x00000000
         |  32768 B   |  32768 B   |   - 0x0000ffff
         +------------+------------+
 bank 1  |  game.1.0  |  game.1.1  |  0x00010000
         |  32768 B   |  32768 B   |   - 0x0001ffff
         +------------+------------+

Which holds 131072 bytes altogether, laid out like this:

         0x00000000                                              0x00020000
         +----------------+----------------+----------------+----------------+
         |#...............|#...............|#...............|#...............|
         +----------------+----------------+----------------+----------------+
         # 700 bytes of data   . 32068 bytes of 0xff pad   x 4 copies of 32768 bytes

Dry run, nothing written.
```

Read the grid the way the board is wired. Columns are byte lanes: with
four 8-bit ROMs on a 16-bit bus, `game.0.0` carries the even bytes and
`game.0.1` the odd ones, so consecutive bytes of the binary alternate
between the two chips. Rows are banks, which take the address space one
after another - bank 1 starts where bank 0 ends.

The bar underneath is the whole address space. `--paduptosize` says how
big one copy of the input is; anything left over in a copy is filled with
`--pad` (`0xff` by default, what an erased EPROM reads as) and the copy is
repeated until the parts are full. Leave `--paduptosize` off and you get a
single copy padded out to the total. An input bigger than a copy gets
truncated, and romjak says so before it does it.

### What --paduptosize is for

Say the board wants two 8-bit EPROMs on a 16-bit bus, but only decodes 14
address lines - it can see 16KB of the pair. You have 27C256s, which are
32KB each, so the pair is 64KB and the top two address lines are not
connected to anything. Whatever those lines happen to float or strap to,
the CPU has to find your program.

So put a copy in all four quarters. The program is 10240 bytes, one
quarter is 16KB:

```
$ romjak split -n --numroms=2 --romsize=32KB --paduptosize=16KB prog.bin prog
Splitting prog.bin (10240 bytes) over 2 ROMs of 32768 bytes, 1 bank(s), 8-bit each:

            byte +0      byte +1
         +------------+------------+
 bank 0  |   prog.0   |   prog.1   |  0x00000000
         |  32768 B   |  32768 B   |   - 0x0000ffff
         +------------+------------+

Which holds 65536 bytes altogether, laid out like this:

         0x00000000                                              0x00010000
         +----------------+----------------+----------------+----------------+
         |##########......|##########......|##########......|##########......|
         +----------------+----------------+----------------+----------------+
         # 10240 bytes of data   . 6144 bytes of 0xff pad   x 4 copies of 16384 bytes

Dry run, nothing written.
```

Four copies, each 10240 bytes of program followed by 6144 bytes of `0xff`
so the next copy starts exactly on a 16KB boundary. Miss `--paduptosize`
off and you would get one copy at the bottom and 54KB of `0xff` above it,
which works only if those two address lines happen to come up low.

Outputs are named `<basename>.<rom>` for one bank and
`<basename>.<bank>.<rom>` for several. Leave the basename off and it comes
from the input path, so `roms/game.bin` gives `game.0`, `game.1`, ...

## Joining

```
romjak join [--romwidth=<n>] [--rombanks=<n>] [--trim=<n>] [--pad=<byte>]
            [-n] -o <file> <rom>...
```

The inverse. Give it the images in bus order within a bank and then bank
by bank - the order `split` wrote them - and it interleaves them back into
one binary:

```
romjak join --rombanks=2 --trim=700 -o game.bin game.0.0 game.0.1 game.1.0 game.1.1
```

Most of the geometry comes off the files themselves, so only `--romwidth`
and `--rombanks` need telling, because nothing in an image records them.
It draws the same grid before it starts, which is the quickest way to spot
an argument in the wrong place.

Padding is not recoverable from the images either. join reports how long
the run of pad bytes at the tail is, and `--trim=<n>` cuts the output back
to the real data.

## Limits

Up to 16 ROMs, up to 4 banks, ROM widths from 8 to 32 bits. `--romsize`
and `--paduptosize` have to be multiples of the ROM stride (the width in
bytes), and the total has to be a whole number of copies.
