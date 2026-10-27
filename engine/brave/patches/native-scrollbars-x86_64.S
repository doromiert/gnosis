/* SPDX-License-Identifier: GPL-3.0-or-later
 * Pinned Brave 1.97.56 / Chromium 155.0.8059.40, x86-64 SysV ABI.
 * Linked at 0x7376d80 in verified RX alignment padding. No new executable mapping.
 * as native-scrollbars-x86_64.S -o /tmp/native-scrollbars.o
 * ld -Ttext=0x7376d80 --defsym=original_color=0x5638590 \
 *   --defsym=original_track=0x5636180 --defsym=original_thumb=0xe1fa2f0 \
 *   /tmp/native-scrollbars.o -o /tmp/native-scrollbars.elf
 * objcopy -O binary --only-section=.text /tmp/native-scrollbars.elf /tmp/native-scrollbars.bin
 */
.intel_syntax noprefix
.text
.global thumb_color, track_inset, track, proportions
thumb_color:
  /* Private track-paint params; real is_hovering is a bool (0 or 1). */
  cmp byte ptr [rcx], 0x7f
  je .track_color
  /* Preserve authored scrollbar-color and forced-color overrides. */
  cmp byte ptr [rcx+8], 0
  jne original_color
  cmp byte ptr [rcx+16], 0
  jne original_color
  push rdx
  call original_color
  pop rdx
  /* Preserve opaque black/white accessibility palettes. */
  cmp eax, 0xff000000
  je .done
  cmp eax, 0xffffffff
  je .done
  mov eax, 0x33ffffff
  cmp edx, 1
  je .hover
  cmp edx, 3
  je .pressed
  test edx, edx
  jne .done
  xor eax, eax
.done:
  ret
.hover:
  mov eax, 0x66ffffff
  ret
.pressed:
  mov eax, 0x99ffffff
  ret
.track_color:
  mov eax, 0x1affffff
  ret
.org 0x80, 0xcc
track_inset:
  mov eax, 8
  ret
.org 0x90, 0xcc
track:
  /* Track ABI: r9=extra params, stack=rect,forced_colors,contrast. */
  cmp byte ptr [rsp+16], 0
  jne original_track
  cmp dword ptr [rsp+24], 1
  je original_track
  cmp byte ptr [r9+28], 0
  jne original_track
  push rbp
  mov rbp, rsp
  sub rsp, 64
  /* Reuse Chromium's antialiased rounded-rectangle painter with private params. */
  pxor xmm0, xmm0
  movdqu [rsp+8], xmm0
  movq [rsp+24], xmm0
  mov byte ptr [rsp+8], 0x7f
  lea rax, [rsp+8]
  mov [rsp], rax
  mov rax, [rbp+16]
  movdqu xmm0, [rax]
  movdqu [rsp+32], xmm0
  cmp ecx, 16
  je .horizontal
  add dword ptr [rsp+36], 9
  sub dword ptr [rsp+44], 18
  jmp .paint
.horizontal:
  add dword ptr [rsp+32], 9
  sub dword ptr [rsp+40], 18
.paint:
  cmp dword ptr [rsp+40], 0
  jle .return
  cmp dword ptr [rsp+44], 0
  jle .return
  lea r9, [rsp+32]
  call original_thumb
.return:
  mov rsp, rbp
  pop rbp
  ret
.org 0x180, 0xcc
/* Separate lookup so unrelated constants in Chromium remain untouched. */
proportions:
  .float 1.0, 1.0
