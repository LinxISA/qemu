OUTPUT_ARCH(linx)

    ENTRY( _start )

    MEMORY
    {
      ram    : ORIGIN = 0x80000000, LENGTH = 0x300000
    }

    PHDRS
    {
      text PT_LOAD;
      data PT_LOAD;
      bss PT_LOAD;
      eh_frame PT_LOAD;
    }

    SECTIONS
    {
      .text : {
        PROVIDE(_text_start = .);
        *(.text.init) *(.text .text.*)
        PROVIDE(_text_end = .);
      } >ram AT>ram :text

      .tohost : {
        PROVIDE(_tohost_start = .);
        *(.stohost .stohost.*) *(.tohost .tohost.*)
        PROVIDE(_tohost_end = .);
      } >ram AT>ram :text

      .rodata : {
        PROVIDE(_rodata_start = .);
        *(.rodata .rodata.*)
        PROVIDE(_rodata_end = .);
      } >ram AT>ram :text

      .data : {
        . = ALIGN(0x4000);
        PROVIDE(_data_start = .);
        *(.sdata .sdata.*) *(.data .data.*)
        PROVIDE(_data_end = .);
      } >ram AT>ram :data

      .bss :{
        . = ALIGN(0x4000);
        PROVIDE(_bss_start = .);
        *(.sbss .sbss.*) *(.bss .bss.*)
        PROVIDE(_bss_end = .);
      } >ram AT>ram :bss

      .eh_frame :{
        PROVIDE(_eh_frame_start = .);
        *(.seh_frame .seh_frame.*) *(.eh_frame .eh_frame.*)
        PROVIDE(_eh_frame_end = .);
      } >ram AT>ram :eh_frame

      PROVIDE(_memory_start = ORIGIN(ram));
      PROVIDE(_memory_end = ORIGIN(ram) + LENGTH(ram));
    }
