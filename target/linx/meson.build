# FIXME extra_args should accept files()
dir = meson.current_source_dir()

gen = [
  decodetree.process('block16.decode', extra_args: ['--static-decode=decode_block16', '--insnwidth=16']),
  decodetree.process('block32.decode', extra_args: ['--static-decode=decode_block32', '--insnwidth=32']),
  decodetree.process('block48.decode', extra_args: ['--static-decode=decode_block48', '--insnwidth=64']),
  decodetree.process('block32_private_fvec.decode', extra_args: ['--static-decode=decode_block32_private_fvec', '--insnwidth=64']),
]

linx_ss = ss.source_set()
linx_ss.add(gen)
linx_ss.add(files(
  'cpu.c',
  'cpu_helper.c',
  'csr.c',
  'gdbstub.c',
  'op_helper.c',
  'fpu_helper.c',
  'translate.c',
  'disas.c',
))

linx_softmmu_ss = ss.source_set()
linx_softmmu_ss.add(files(
  'arch_dump.c',
  'monitor.c',
  'machine.c'
))

target_arch += {'linx': linx_ss}
target_softmmu_arch += {'linx': linx_softmmu_ss}
