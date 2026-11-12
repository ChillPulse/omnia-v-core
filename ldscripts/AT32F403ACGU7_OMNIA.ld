/*
 * AT32F403ACGU7_OMNIA.ld
 * Кастомный линкер-скрипт для OMNIA Studio DSP (AT32F403ACGU7: 1024 KB Flash, 96+128 = 224 KB SRAM).
 *
 * КРИТИЧЕСКАЯ ИНЖЕНЕРНАЯ ЗАЩИТА ПЕРВОГО СТАРТА:
 * С завода в чипе AT32F403ACGU7 байт EOPB0 = 0xFF (открыто только 96 КБ SRAM: 0x20000000..0x20018000).
 * Поэтому стек (_estack = 0x20018000), .data и .bss размещаются в базовых 96 КБ, чтобы чип гарантированно
 * стартовал без HardFault даже из заводской упаковки!
 * Расширенные 128 КБ SRAM (0x20018000..0x20038000) выделены в секцию .sram_ext (NOLOAD) под буферы
 * 3D-декоррелятора AETHER, Look-Ahead True-Peak лимитера и буфер Smart OTA — они активируются и обнуляются
 * только после проверки EOPB0 = 0xFE и успешной проверки памяти; прошивка не меняет USD.
 */

ENTRY(Reset_Handler)

_estack = 0x20018000;
_Min_Heap_Size  = 0x400;
_Min_Stack_Size = 0x1000;

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM     (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  RAM_EXT (xrw) : ORIGIN = 0x20018000, LENGTH = 128K
  SPIM    (rx)  : ORIGIN = 0x08400000, LENGTH = 16384K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  _spim_init_base = LOADADDR(.spim);
  _spim_init_length = SIZEOF(.spim);

  .spim :
  {
    . = ALIGN(4);
    _spim_start = .;
    *(.spim)
    *(.spim*)
    . = ALIGN(4);
    _spim_end = .;
  } >SPIM

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Расширенная область 128 КБ SRAM (0x20018000..0x20038000) под линии задержки AETHER/Limiter и OTA */
  .sram_ext (NOLOAD) :
  {
    . = ALIGN(8);
    _ssram_ext = .;
    *(.sram_ext)
    *(.sram_ext*)
    . = ALIGN(8);
    _esram_ext = .;
  } >RAM_EXT

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
