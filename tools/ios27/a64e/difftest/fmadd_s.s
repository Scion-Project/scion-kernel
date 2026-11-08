	.text
	.align 2
	.globl _sweet_a64e_fp_fmadd_s
_sweet_a64e_fp_fmadd_s:
	mrs	x9, FPCR
	mrs	x10, FPSR
	mov	w3, w3
	msr	FPCR, x3
	msr	FPSR, xzr
	fmov	s0, w0
	fmov	s1, w1
	fmov	s2, w2
	fmadd	s0, s1, s2, s0
	fmov	w0, s0
	mrs	x12, FPSR
	ldr	w13, [x4]
	orr	w13, w13, w12
	str	w13, [x4]
	msr	FPCR, x9
	msr	FPSR, x10
	ret
