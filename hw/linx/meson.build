linx_ss = ss.source_set()
linx_ss.add(files('boot.c'), fdt)
linx_ss.add(when: 'CONFIG_LINX_NUMA', if_true: files('numa.c'))
linx_ss.add(files('linx_hart.c'))
linx_ss.add(when: 'CONFIG_LINX_VIRT', if_true: files('virt.c'))

hw_arch += {'linx': linx_ss}
