/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef _INTEL_LRC_H_
#define _INTEL_LRC_H_

/* Execlists regs */
#define RING_ELSP(ring)			((ring)->mmio_base+0x230)
#define RING_EXECLIST_STATUS(ring)	((ring)->mmio_base+0x234)
#define RING_CONTEXT_CONTROL(ring)	((ring)->mmio_base+0x244)
#define RING_CONTEXT_STATUS_BUF(ring)	((ring)->mmio_base+0x370)
#define RING_CONTEXT_STATUS_PTR(ring)	((ring)->mmio_base+0x3a0)

/* Execlists */
int intel_engine_enable_execlists(struct intel_engine_cs *engine);
void intel_execlists_irq_handler(struct intel_engine_cs *engine);

#endif /* _INTEL_LRC_H_ */
