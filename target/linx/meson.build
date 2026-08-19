gen = [
  decodetree.process('insn16.decode', extra_args: ['--static-decode=decode_insn16', '--insnwidth=16']),
  decodetree.process('insn32.decode', extra_args: ['--static-decode=decode_insn32', '--insnwidth=32']),
  decodetree.process('insn48.decode', extra_args: ['--static-decode=decode_insn48', '--insnwidth=64']),
  decodetree.process('insn64.decode', extra_args: ['--static-decode=decode_insn64', '--insnwidth=64']),
]

linx_ss = ss.source_set()
linx_ss.add(gen)
linx_ss.add(files(
  'cpu.c',
  'first_use.c',
  'helper.c',
  'tile_cube_058.c',
  'translate.c',
))

target_arch += {'linx': linx_ss}
