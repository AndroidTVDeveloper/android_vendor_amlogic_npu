/****************************************************************************
*
*    Copyright (c) 2018 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************/
#ifndef _VSI_NN_OP_SPACE2BATCH_H
#define _VSI_NN_OP_SPACE2BATCH_H

#include "vsi_nn_types.h"

typedef struct _vsi_nn_space2batch_lcl_data_t
{
    vsi_nn_tensor_t *block_size_tensor;
    vsi_nn_tensor_t *pad_tensor;
} vsi_nn_space2batch_lcl_data_t;

typedef struct _vsi_nn_space2batch_param
{
    /* local data must be the first. */
    vsi_nn_space2batch_lcl_data_t local;

    uint32_t block_size; //only for old code
    uint32_t *block_size_2;
    uint32_t block_size_2_num;
    uint32_t pad[4]; // [top, bottom, left, right]
} vsi_nn_space2batch_param;

#endif

