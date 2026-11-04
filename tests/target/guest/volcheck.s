| volcheck: guest tool for tests/target/quick.mjs. Writes a stamped file to a volume through
| dos.library, reads it back, compares, and leaves the outcome in low memory for the harness.
|
|   volcheck <volume> <megabytes> [file] [w|v|i]    w write only, v verify only, i Info only
|   volcheck serve                                  stay resident, take commands from $180
|
| Results at $100, all longs:
|   $100 magic  'VCDN' finished, 'BUSY' running
|   $104 error  IoErr() at the failure, 0 if none
|   $108 phase  0 ok, 1 Lock/Info, 2 open to write, 3 write, 4 open to read, 5 read, 6 compare
|   $10C bytes written   $110 bytes read   $114 bytes compared equal
|   $118 sectors that differed   $11C first differing file sector, -1 if none
|   $120 id_NumBlocks   $124 id_NumBlocksUsed (before)   $128 id_BytesPerBlock
|   $12C id_DiskType    $130 id_DiskState                $134 command number done
|   $138 id_NumBlocksUsed (after)   $13C tag (path hash, see below)
| Serve mailbox: $17C 'SRDY' when listening
|   $180 command number (written last)   $184 megabytes   $188 flags 1 write only, 2 verify only,
|   4 Info only, $80 exit   $190 volume, NUL-terminated   $1A0 file, NUL-terminated
|
| Every 512-byte sector of the file holds 'VCHK', tag, file sector number, then 125 longs starting
| at number^tag^$5AA55AA5 and rising by $01010101. FFS keeps file data in whole blocks, so the host
| can find each sector in the hardfile. tag = hash of "volume:file": h = h*31 + c, per byte.
    .text
    .equ RES, 0x100
    .equ R_MAGIC, RES+0x00
    .equ R_ERR,   RES+0x04
    .equ R_PHASE, RES+0x08
    .equ R_WROTE, RES+0x0C
    .equ R_READ,  RES+0x10
    .equ R_EQUAL, RES+0x14
    .equ R_BAD,   RES+0x18
    .equ R_FIRST, RES+0x1C
    .equ R_BLOCKS,RES+0x20
    .equ R_USED,  RES+0x24
    .equ R_BSIZE, RES+0x28
    .equ R_DTYPE, RES+0x2C
    .equ R_STATE, RES+0x30
    .equ R_DONE,  RES+0x34
    .equ R_AFTER, RES+0x38
    .equ R_TAG,   RES+0x3C
    .equ M_READY, 0x17C
    .equ M_SEQ,   0x180
    .equ M_MB,    0x184
    .equ M_FLAGS, 0x188
    .equ M_VOL,   0x190
    .equ M_FILE,  0x1A0
    .equ F_WRITE, 1
    .equ F_VERIFY,2
    .equ F_INFO,  4
    .equ F_EXIT,  0x80
    .equ CHUNK,   0x40000
    .equ W_INFO,  0
    .equ W_PATH,  64
    .equ W_ROOT,  192
    .equ W_ARGS,  256
    .equ W_BUF1,  512
    .equ W_BUF2,  512+CHUNK
    .equ W_SIZE,  512+2*CHUNK
    .equ VCHK,    0x5643484B

| Register use throughout: a4 workspace, a5 dos.library, a6 exec or dos around calls,
| a3 open file, d4 bytes left, d5 file sector, d6 tag, d7 flags.
    .globl start
start:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l %a0,%a2
    move.l %d0,%d2
    move.l 4.w,%a6
    lea dosname(%pc),%a1
    moveq #36,%d0
    jsr -552(%a6)              | OpenLibrary
    move.l %d0,%a5
    tst.l %d0
    beq quit
    move.l #W_SIZE,%d0
    moveq #1,%d1
    jsr -198(%a6)              | AllocMem MEMF_PUBLIC
    move.l %d0,%a4
    tst.l %d0
    beq closedos

    | Copy the argument line, turning separators into NULs.
    lea W_ARGS(%a4),%a0
    move.l #254,%d1
    cmp.l %d1,%d2
    bls 1f
    move.l %d1,%d2
1:  move.l %d2,%d1
    subq.l #1,%d1
    bmi 4f
2:  move.b (%a2)+,%d0
    cmp.b #' ',%d0
    bhi 3f
    moveq #0,%d0
3:  move.b %d0,(%a0)+
    dbf %d1,2b
4:  clr.b (%a0)+
    st (%a0)+                  | end marker for nexttok
    clr.b (%a0)+
    lea W_ARGS(%a4),%a2
    bsr nexttok                | a0 = first word
    tst.b (%a0)
    beq usage
    lea servestr(%pc),%a1
    bsr streq
    bne serve

    | One shot: volume megabytes [file] [mode]
    move.l %a0,%d3             | volume
    bsr nexttok
    bsr atoi
    move.l %d0,%d5             | megabytes
    bsr nexttok
    lea deffile(%pc),%a1
    tst.b (%a0)
    beq 5f
    move.l %a0,%a1
5:  move.l %d3,%a0
    bsr buildpath
    moveq #0,%d7
    bsr nexttok
    move.b (%a0),%d0
    cmp.b #'w',%d0
    bne 6f
    moveq #F_WRITE,%d7
6:  cmp.b #'v',%d0
    bne 7f
    moveq #F_VERIFY,%d7
7:  cmp.b #'i',%d0
    bne 8f
    moveq #F_INFO,%d7
8:  move.l %d5,%d4
    bsr check
    bra finish

serve:
    lea M_SEQ.w,%a0
    moveq #31,%d0
1:  clr.l (%a0)+
    dbf %d0,1b
    move.l #0x53524459,M_READY.w   | 'SRDY'
    moveq #0,%d3               | last command done
2:  move.l M_SEQ.w,%d0
    cmp.l %d3,%d0
    beq 3f
    move.l %d0,%d3
    moveq #0,%d7
    move.b (M_FLAGS+3).w,%d7
    btst #7,%d7
    bne finish
    lea M_VOL.w,%a0
    lea M_FILE.w,%a1
    bsr buildpath
    move.l M_MB.w,%d4
    bsr check
    move.l %d3,R_DONE.w
    move.l #0x5643444E,R_MAGIC.w   | 'VCDN', written last
    bra 2b
3:  moveq #5,%d1
    move.l %a5,%a6
    jsr -198(%a6)              | Delay
    bra 2b

usage:
    move.l #0x5643444E,R_MAGIC.w
    move.l #-1,R_PHASE.w
finish:
    move.l 4.w,%a6
    move.l %a4,%a1
    move.l #W_SIZE,%d0
    jsr -210(%a6)              | FreeMem
closedos:
    move.l 4.w,%a6
    move.l %a5,%a1
    jsr -414(%a6)              | CloseLibrary
quit:
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    moveq #0,%d0
    rts

| Next word of the argument copy: a2 walks, a0 returns it (empty string at the end).
nexttok:
    move.b (%a2),%d0
    bne 1f
    addq.l #1,%a2
    bra nexttok
1:  cmp.b #-1,%d0
    bne 2f
    lea 1(%a2),%a0
    rts
2:  move.l %a2,%a0
3:  tst.b (%a2)+
    bne 3b
    subq.l #1,%a2
    rts

| Z clear if the strings at a0 and a1 match. Keeps a0.
streq:
    move.l %a0,-(%sp)
1:  move.b (%a0)+,%d0
    cmp.b (%a1)+,%d0
    bne 2f
    tst.b %d0
    bne 1b
    move.l (%sp)+,%a0
    moveq #1,%d0
    rts
2:  move.l (%sp)+,%a0
    moveq #0,%d0
    rts

atoi:
    moveq #0,%d0
    moveq #0,%d1
1:  move.b (%a0)+,%d1
    sub.b #'0',%d1
    cmp.b #9,%d1
    bhi 2f
    add.l %d0,%d0
    move.l %d0,-(%sp)
    lsl.l #2,%d0
    add.l (%sp)+,%d0
    add.l %d1,%d0
    bra 1b
2:  rts

| a0 volume (with or without colon), a1 file. Fills W_ROOT and W_PATH, sets d6 to the tag.
buildpath:
    move.l %a2,-(%sp)
    lea W_ROOT(%a4),%a2
1:  move.b (%a0)+,%d0
    beq 2f
    cmp.b #':',%d0
    beq 2f
    move.b %d0,(%a2)+
    bra 1b
2:  move.b #':',(%a2)+
    clr.b (%a2)
    lea W_ROOT(%a4),%a0
    lea W_PATH(%a4),%a2
3:  move.b (%a0)+,(%a2)+
    bne 3b
    subq.l #1,%a2
4:  move.b (%a1)+,(%a2)+
    bne 4b
    lea W_PATH(%a4),%a0
    moveq #0,%d6
    moveq #0,%d1
5:  move.b (%a0)+,%d1
    beq 6f
    move.l %d6,%d0
    lsl.l #5,%d0
    sub.l %d6,%d0
    add.l %d1,%d0
    move.l %d0,%d6
    bra 5b
6:  move.l (%sp)+,%a2
    rts

| Fill a0 with d1 sectors of the pattern, starting at file sector d0.
gen:
    movem.l %d2-%d3,-(%sp)
    subq.l #1,%d1
1:  move.l #VCHK,(%a0)+
    move.l %d6,(%a0)+
    move.l %d0,(%a0)+
    move.l %d0,%d2
    eor.l %d6,%d2
    eor.l #0x5AA55AA5,%d2
    moveq #124,%d3
2:  move.l %d2,(%a0)+
    add.l #0x01010101,%d2
    dbf %d3,2b
    addq.l #1,%d0
    dbf %d1,1b
    movem.l (%sp)+,%d2-%d3
    rts

| Lock/Info into the result block. d0 = result offset for "used". Returns Z set on failure.
info:
    movem.l %d2/%a2,-(%sp)
    move.l %d0,%a2
    move.l %a5,%a6
    lea W_ROOT(%a4),%a0
    move.l %a0,%d1
    moveq #-2,%d2              | ACCESS_READ
    jsr -84(%a6)               | Lock
    tst.l %d0
    beq 9f
    move.l %d0,-(%sp)
    move.l %d0,%d1
    lea W_INFO(%a4),%a0
    move.l %a0,%d2
    jsr -114(%a6)              | Info
    move.l (%sp)+,%d1
    move.l %d0,-(%sp)
    jsr -90(%a6)               | UnLock
    move.l (%sp)+,%d0
    beq 9f
    lea W_INFO(%a4),%a0
    move.l 8(%a0),%d0
    move.l %d0,R_STATE.w
    move.l 12(%a0),R_BLOCKS.w
    move.l 16(%a0),(%a2)
    move.l 20(%a0),R_BSIZE.w
    move.l 24(%a0),R_DTYPE.w
    moveq #1,%d0
    movem.l (%sp)+,%d2/%a2
    rts
9:  moveq #0,%d0
    movem.l (%sp)+,%d2/%a2
    rts

| Run one check. d4 megabytes, d6 tag, d7 flags, paths built. Leaves the results.
check:
    movem.l %d2-%d3/%a2-%a3,-(%sp)
    move.l #0x42555359,R_MAGIC.w   | 'BUSY'
    lea R_ERR.w,%a0
    moveq #13,%d0
1:  clr.l (%a0)+
    dbf %d0,1b
    move.l #-1,R_FIRST.w
    move.l %d6,R_TAG.w
    moveq #20,%d0
    lsl.l %d0,%d4              | bytes
    move.l #R_USED,%d0
    bsr info
    bne 2f
    moveq #1,%d0
    bra fail
2:  btst #2,%d7
    bne done

    btst #1,%d7
    bne verify
    | ---- write
    lea W_PATH(%a4),%a0
    move.l %a0,%d1
    move.l #1006,%d2           | MODE_NEWFILE
    jsr -30(%a6)               | Open
    move.l %d0,%a3
    tst.l %d0
    bne 3f
    moveq #2,%d0
    bra fail
3:  move.l %d4,%d3
    moveq #0,%d5
4:  tst.l %d3
    beq 6f
    move.l %d3,%d2
    cmp.l #CHUNK,%d2
    bls 5f
    move.l #CHUNK,%d2
5:  lea W_BUF1(%a4),%a0
    move.l %d5,%d0
    move.l %d2,%d1
    moveq #9,%d4
    lsr.l %d4,%d1
    add.l %d1,%d5
    bsr gen
    move.l %d3,-(%sp)
    move.l %d2,%d3
    move.l %a3,%d1
    lea W_BUF1(%a4),%a0
    move.l %a0,%d2
    jsr -48(%a6)               | Write
    move.l %d3,%d2
    move.l (%sp)+,%d3
    cmp.l %d0,%d2
    bne 7f
    add.l %d2,R_WROTE.w
    sub.l %d2,%d3
    bra 4b
7:  tst.l %d0
    bmi 8f
    add.l %d0,R_WROTE.w
8:  jsr -132(%a6)              | IoErr
    move.l %d0,R_ERR.w
    move.l %a3,%d1
    jsr -36(%a6)               | Close
    moveq #3,%d0
    move.l %d0,R_PHASE.w
    bra done
6:  move.l %a3,%d1
    jsr -36(%a6)               | Close
    move.l R_WROTE.w,%d4
    btst #0,%d7
    bne done

verify:
    | ---- read back. d3 bytes left, d4 scratch, d5 file sector.
    lea W_PATH(%a4),%a0
    move.l %a0,%d1
    move.l #1005,%d2           | MODE_OLDFILE
    jsr -30(%a6)               | Open
    move.l %d0,%a3
    tst.l %d0
    bne 2f
    moveq #4,%d0
    bra fail
2:  move.l %d4,%d3
    moveq #0,%d5
3:  tst.l %d3
    beq closeok
    move.l %d3,%d2
    cmp.l #CHUNK,%d2
    bls 4f
    move.l #CHUNK,%d2
4:  move.l %d3,-(%sp)
    move.l %d2,%d3
    move.l %a3,%d1
    move.l %a4,%d2
    add.l #W_BUF2,%d2
    jsr -42(%a6)               | Read
    move.l %d3,%d2
    move.l (%sp)+,%d3
    cmp.l %d0,%d2
    beq 6f
    tst.l %d0
    bmi 5f
    add.l %d0,R_READ.w
5:  jsr -132(%a6)              | IoErr
    move.l %d0,R_ERR.w
    move.l %a3,%d1
    jsr -36(%a6)               | Close
    moveq #5,%d0
    move.l %d0,R_PHASE.w
    bra done
6:  add.l %d2,R_READ.w
    lea W_BUF1(%a4),%a0
    move.l %d5,%d0
    move.l %d2,%d1
    moveq #9,%d4
    lsr.l %d4,%d1
    move.l %d1,-(%sp)          | sectors this round
    bsr gen
    lea W_BUF1(%a4),%a0
    move.l %a4,%a1
    add.l #W_BUF2,%a1
    move.l (%sp)+,%d4
    subq.l #1,%d4
7:  moveq #127,%d0
    moveq #0,%d1
8:  cmpm.l (%a0)+,(%a1)+
    beq 9f
    moveq #1,%d1
9:  dbf %d0,8b
    tst.l %d1
    beq 10f
    addq.l #1,R_BAD.w
    tst.l R_FIRST.w
    bpl 11f
    move.l %d5,R_FIRST.w
    bra 11f
10: add.l #512,R_EQUAL.w
11: addq.l #1,%d5
    dbf %d4,7b
    sub.l %d2,%d3
    bra 3b
closeok:
    move.l %a3,%d1
    jsr -36(%a6)               | Close
    tst.l R_BAD.w
    beq done
    moveq #6,%d0
    move.l %d0,R_PHASE.w
    bra done

fail:
    move.l %d0,R_PHASE.w
    move.l %a5,%a6
    jsr -132(%a6)              | IoErr
    move.l %d0,R_ERR.w
done:
    moveq #1,%d0
    cmp.l R_PHASE.w,%d0        | no second try, and no second requester, if Lock failed
    beq 1f
    move.l #R_AFTER,%d0
    bsr info
1:
    move.l #0x5643444E,R_MAGIC.w   | 'VCDN'
    movem.l (%sp)+,%d2-%d3/%a2-%a3
    rts

dosname:  .asciz "dos.library"
servestr: .asciz "serve"
deffile:  .asciz "volcheck.dat"
    .even
