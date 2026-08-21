project('romjak', 'c')

argtable3 = dependency('argtable3')

romjak = executable('romjak', 'romjak.c', dependencies : argtable3)

test('roundtrip', find_program('tests/roundtrip.sh'), args : [romjak])

