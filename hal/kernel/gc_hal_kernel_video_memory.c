/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2014 - 2018 Vivante Corporation
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
*****************************************************************************
*
*    The GPL License (GPL)
*
*    Copyright (C) 2014 - 2018 Vivante Corporation
*
*    This program is free software; you can redistribute it and/or
*    modify it under the terms of the GNU General Public License
*    as published by the Free Software Foundation; either version 2
*    of the License, or (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*****************************************************************************
*
*    Note: This software is released under dual MIT and GPL licenses. A
*    recipient may use this file under the terms of either the MIT license or
*    GPL License. If you wish to use only one license not the other, you can
*    indicate your decision by deleting one of the above license notices in your
*    version of this file.
*
*****************************************************************************/


#include "gc_hal_kernel_precomp.h"

#define _GC_OBJ_ZONE    gcvZONE_VIDMEM

/******************************************************************************\
******************************* Private Functions ******************************
\******************************************************************************/

/*******************************************************************************
**
**  _Split
**
**  Split a node on the required byte boundary.
**
**  INPUT:
**
**      gckOS Os
**          Pointer to an gckOS object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to the node to split.
**
**      gctSIZE_T Bytes
**          Number of bytes to keep in the node.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gctBOOL
**          gcvTRUE if the node was split successfully, or gcvFALSE if there is an
**          error.
**
*/
static gctBOOL
_Split(
    IN gckOS Os,
    IN gcuVIDMEM_NODE_PTR Node,
    IN gctSIZE_T Bytes
    )
{
    gcuVIDMEM_NODE_PTR node;
    gctPOINTER pointer = gcvNULL;

    /* Make sure the byte boundary makes sense. */
    if ((Bytes <= 0) || (Bytes > Node->VidMem.bytes))
    {
        return gcvFALSE;
    }

    /* Allocate a new gcuVIDMEM_NODE object. */
    if (gcmIS_ERROR(gckOS_Allocate(Os,
                                   gcmSIZEOF(gcuVIDMEM_NODE),
                                   &pointer)))
    {
        /* Error. */
        return gcvFALSE;
    }

    node = pointer;

    /* Initialize gcuVIDMEM_NODE structure. */
    node->VidMem.offset    = Node->VidMem.offset + Bytes;
    node->VidMem.bytes     = Node->VidMem.bytes  - Bytes;
    node->VidMem.alignment = 0;
    node->VidMem.locked    = 0;
    node->VidMem.parent    = Node->VidMem.parent;
    node->VidMem.pool      = Node->VidMem.pool;
#ifdef __QNXNTO__
    node->VidMem.processID = 0;
    node->VidMem.logical   = gcvNULL;
#endif
    node->VidMem.kvaddr    = gcvNULL;

    /* Insert node behind specified node. */
    node->VidMem.next = Node->VidMem.next;
    node->VidMem.prev = Node;
    Node->VidMem.next = node->VidMem.next->VidMem.prev = node;

    /* Insert free node behind specified node. */
    node->VidMem.nextFree = Node->VidMem.nextFree;
    node->VidMem.prevFree = Node;
    Node->VidMem.nextFree = node->VidMem.nextFree->VidMem.prevFree = node;

    /* Adjust size of specified node. */
    Node->VidMem.bytes = Bytes;

    /* Success. */
    return gcvTRUE;
}

/*******************************************************************************
**
**  _Merge
**
**  Merge two adjacent nodes together.
**
**  INPUT:
**
**      gckOS Os
**          Pointer to an gckOS object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to the first of the two nodes to merge.
**
**  OUTPUT:
**
**      Nothing.
**
*/
static gceSTATUS
_Merge(
    IN gckOS Os,
    IN gcuVIDMEM_NODE_PTR Node
    )
{
    gcuVIDMEM_NODE_PTR node;
    gceSTATUS status;

    /* Save pointer to next node. */
    node = Node->VidMem.next;

    /* This is a good time to make sure the heap is not corrupted. */
    if (Node->VidMem.offset + Node->VidMem.bytes != node->VidMem.offset)
    {
        /* Corrupted heap. */
        gcmkASSERT(
            Node->VidMem.offset + Node->VidMem.bytes == node->VidMem.offset);
        return gcvSTATUS_HEAP_CORRUPTED;
    }

    /* Adjust byte count. */
    Node->VidMem.bytes += node->VidMem.bytes;

    /* Unlink next node from linked list. */
    Node->VidMem.next     = node->VidMem.next;
    Node->VidMem.nextFree = node->VidMem.nextFree;

    Node->VidMem.next->VidMem.prev         =
    Node->VidMem.nextFree->VidMem.prevFree = Node;

    /* Free next node. */
    status = gcmkOS_SAFE_FREE(Os, node);
    return status;
}

/******************************************************************************\
******************************* gckVIDMEM API Code ******************************
\******************************************************************************/

/*******************************************************************************
**
**  gckVIDMEM_Construct
**
**  Construct a new gckVIDMEM object.
**
**  INPUT:
**
**      gckOS Os
**          Pointer to an gckOS object.
**
**      gctPHYS_ADDR_T PhysicalBase
**          Base physical address for the video memory heap.
**
**      gctSIZE_T Bytes
**          Number of bytes in the video memory heap.
**
**      gctSIZE_T Threshold
**          Minimum number of bytes beyond am allocation before the node is
**          split.  Can be used as a minimum alignment requirement.
**
**      gctSIZE_T BankSize
**          Number of bytes per physical memory bank.  Used by bank
**          optimization.
**
**  OUTPUT:
**
**      gckVIDMEM * Memory
**          Pointer to a variable that will hold the pointer to the gckVIDMEM
**          object.
*/
gceSTATUS
gckVIDMEM_Construct(
    IN gckOS Os,
    IN gctPHYS_ADDR_T PhysicalBase,
    IN gctSIZE_T Bytes,
    IN gctSIZE_T Threshold,
    IN gctSIZE_T BankSize,
    OUT gckVIDMEM * Memory
    )
{
    gckVIDMEM memory = gcvNULL;
    gceSTATUS status;
    gcuVIDMEM_NODE_PTR node;
    gctINT i, banks = 0;
    gctPOINTER pointer = gcvNULL;
    gctUINT32 heapBytes;
    gctUINT32 bankSize;
    gctUINT32 base = 0;

    gcmkHEADER_ARG("Os=0x%x PhysicalBase=%12llx Bytes=%lu Threshold=%lu "
                   "BankSize=%lu",
                   Os, PhysicalBase, Bytes, Threshold, BankSize);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Os, gcvOBJ_OS);
    gcmkVERIFY_ARGUMENT(Bytes > 0);
    gcmkVERIFY_ARGUMENT(Memory != gcvNULL);

    gcmkSAFECASTSIZET(heapBytes, Bytes);
    gcmkSAFECASTSIZET(bankSize, BankSize);

    /* Allocate the gckVIDMEM object. */
    gcmkONERROR(gckOS_Allocate(Os, gcmSIZEOF(struct _gckVIDMEM), &pointer));
    gckOS_ZeroMemory(pointer, gcmSIZEOF(struct _gckVIDMEM));

    memory = pointer;

    /* Initialize the gckVIDMEM object. */
    memory->object.type = gcvOBJ_VIDMEM;
    memory->os          = Os;

    /* Set video memory heap information. */
    memory->physicalBase = PhysicalBase;
    memory->bytes        = heapBytes;
    memory->freeBytes    = heapBytes;
    memory->minFreeBytes = heapBytes;
    memory->capability   = ~0u;
    memory->threshold    = Threshold;
    memory->mutex        = gcvNULL;

    /* Walk all possible banks. */
    for (i = 0; i < gcmCOUNTOF(memory->sentinel); ++i)
    {
        gctUINT32 bytes;

        if (BankSize == 0)
        {
            /* Use all bytes for the first bank. */
            bytes = heapBytes;
        }
        else
        {
            /* Compute number of bytes for this bank. */
            bytes = gcmALIGN(base + 1, bankSize) - base;

            if (bytes > heapBytes)
            {
                /* Make sure we don't exceed the total number of bytes. */
                bytes = heapBytes;
            }
        }

        if (bytes == 0)
        {
            /* Mark heap is not used. */
            memory->sentinel[i].VidMem.next     =
            memory->sentinel[i].VidMem.prev     =
            memory->sentinel[i].VidMem.nextFree =
            memory->sentinel[i].VidMem.prevFree = gcvNULL;
            continue;
        }

        /* Allocate one gcuVIDMEM_NODE union. */
        gcmkONERROR(gckOS_Allocate(Os, gcmSIZEOF(gcuVIDMEM_NODE), &pointer));

        node = pointer;

        /* Initialize gcuVIDMEM_NODE union. */
        node->VidMem.parent    = memory;

        node->VidMem.next      =
        node->VidMem.prev      =
        node->VidMem.nextFree  =
        node->VidMem.prevFree  = &memory->sentinel[i];

        node->VidMem.offset    = base;
        node->VidMem.bytes     = bytes;
        node->VidMem.alignment = 0;
        node->VidMem.pool      = gcvPOOL_UNKNOWN;

        node->VidMem.locked    = 0;

#ifdef __QNXNTO__
        node->VidMem.processID = 0;
        node->VidMem.logical   = gcvNULL;
#endif

        node->VidMem.kvaddr    = gcvNULL;

        /* Initialize the linked list of nodes. */
        memory->sentinel[i].VidMem.next     =
        memory->sentinel[i].VidMem.prev     =
        memory->sentinel[i].VidMem.nextFree =
        memory->sentinel[i].VidMem.prevFree = node;

        /* Mark sentinel. */
        memory->sentinel[i].VidMem.bytes = 0;

        /* Adjust address for next bank. */
        base += bytes;
        heapBytes   -= bytes;
        banks ++;
    }

    /* Assign all the bank mappings. */
    memory->mapping[gcvVIDMEM_TYPE_COLOR_BUFFER]    = banks - 1;
    memory->mapping[gcvVIDMEM_TYPE_BITMAP]          = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_DEPTH_BUFFER]    = banks - 1;
    memory->mapping[gcvVIDMEM_TYPE_HZ_BUFFER]       = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_TEXTURE]         = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_VERTEX_BUFFER]   = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_INDEX_BUFFER]    = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_TILE_STATUS]     = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_COMMAND]         = banks - 1;

    if (banks > 1) --banks;
    memory->mapping[gcvVIDMEM_TYPE_GENERIC]         = 0;

    memory->mapping[gcvVIDMEM_TYPE_ICACHE]      = 0;
    memory->mapping[gcvVIDMEM_TYPE_TXDESC]      = 0;
    memory->mapping[gcvVIDMEM_TYPE_FENCE]       = 0;
    memory->mapping[gcvVIDMEM_TYPE_TFBHEADER]   = 0;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] INDEX:         bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_INDEX_BUFFER]);
    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] VERTEX:        bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_VERTEX_BUFFER]);
    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] TEXTURE:       bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_TEXTURE]);
    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] RENDER_TARGET: bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_COLOR_BUFFER]);
    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] DEPTH:         bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_DEPTH_BUFFER]);
    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "[GALCORE] TILE_STATUS:   bank %d",
                  memory->mapping[gcvVIDMEM_TYPE_TILE_STATUS]);

    /* Allocate the mutex. */
    gcmkONERROR(gckOS_CreateMutex(Os, &memory->mutex));

    /* Return pointer to the gckVIDMEM object. */
    *Memory = memory;

    /* Success. */
    gcmkFOOTER_ARG("*Memory=0x%x", *Memory);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    if (memory != gcvNULL)
    {
        if (memory->mutex != gcvNULL)
        {
            /* Delete the mutex. */
            gcmkVERIFY_OK(gckOS_DeleteMutex(Os, memory->mutex));
        }

        for (i = 0; i < banks; ++i)
        {
            /* Free the heap. */
            gcmkASSERT(memory->sentinel[i].VidMem.next != gcvNULL);
            gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Os, memory->sentinel[i].VidMem.next));
        }

        /* Free the object. */
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Os, memory));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckVIDMEM_Destroy
**
**  Destroy an gckVIDMEM object.
**
**  INPUT:
**
**      gckVIDMEM Memory
**          Pointer to an gckVIDMEM object to destroy.
**
**  OUTPUT:
**
**      Nothing.
*/
gceSTATUS
gckVIDMEM_Destroy(
    IN gckVIDMEM Memory
    )
{
    gcuVIDMEM_NODE_PTR node, next;
    gctINT i;

    gcmkHEADER_ARG("Memory=0x%x", Memory);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Memory, gcvOBJ_VIDMEM);

    /* Walk all sentinels. */
    for (i = 0; i < gcmCOUNTOF(Memory->sentinel); ++i)
    {
        /* Bail out of the heap is not used. */
        if (Memory->sentinel[i].VidMem.next == gcvNULL)
        {
            break;
        }

        /* Walk all the nodes until we reach the sentinel. */
        for (node = Memory->sentinel[i].VidMem.next;
             node->VidMem.bytes != 0;
             node = next)
        {
            /* Save pointer to the next node. */
            next = node->VidMem.next;

            /* Free the node. */
            gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Memory->os, node));
        }
    }

    /* Free the mutex. */
    gcmkVERIFY_OK(gckOS_DeleteMutex(Memory->os, Memory->mutex));

    /* Mark the object as unknown. */
    Memory->object.type = gcvOBJ_UNKNOWN;

    /* Free the gckVIDMEM object. */
    gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Memory->os, Memory));

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

#if gcdENABLE_BANK_ALIGNMENT

#if !gcdBANK_BIT_START
#error gcdBANK_BIT_START not defined.
#endif

#if !gcdBANK_BIT_END
#error gcdBANK_BIT_END not defined.
#endif
/*******************************************************************************
**  _GetSurfaceBankAlignment
**
**  Return the required offset alignment required to the make BaseAddress
**  aligned properly.
**
**  INPUT:
**
**      gckOS Os
**          Pointer to gcoOS object.
**
**      gceVIDMEM_TYPE Type
**          Type of allocation.
**
**      gctUINT32 BaseAddress
**          Base address of current video memory node.
**
**  OUTPUT:
**
**      gctUINT32_PTR AlignmentOffset
**          Pointer to a variable that will hold the number of bytes to skip in
**          the current video memory node in order to make the alignment bank
**          aligned.
*/
static gceSTATUS
_GetSurfaceBankAlignment(
    IN gckKERNEL Kernel,
    IN gceVIDMEM_TYPE Type,
    IN gctUINT32 BaseAddress,
    OUT gctUINT32_PTR AlignmentOffset
    )
{
    gctUINT32 bank;
    /* To retrieve the bank. */
    static const gctUINT32 bankMask = (0xFFFFFFFF << gcdBANK_BIT_START)
                                    ^ (0xFFFFFFFF << (gcdBANK_BIT_END + 1));

    /* To retrieve the bank and all the lower bytes. */
    static const gctUINT32 byteMask = ~(0xFFFFFFFF << (gcdBANK_BIT_END + 1));

    gcmkHEADER_ARG("Type=%d BaseAddress=0x%x ", Type, BaseAddress);

    /* Verify the arguments. */
    gcmkVERIFY_ARGUMENT(AlignmentOffset != gcvNULL);

    switch (Type)
    {
    case gcvVIDMEM_TYPE_COLOR_BUFFER:
        bank = (BaseAddress & bankMask) >> (gcdBANK_BIT_START);

        /* Align to the first bank. */
        *AlignmentOffset = (bank == 0) ?
            0 :
            ((1 << (gcdBANK_BIT_END + 1)) + 0) -  (BaseAddress & byteMask);
        break;

    case gcvVIDMEM_TYPE_DEPTH_BUFFER:
        bank = (BaseAddress & bankMask) >> (gcdBANK_BIT_START);

        /* Align to the third bank. */
        *AlignmentOffset = (bank == 2) ?
            0 :
            ((1 << (gcdBANK_BIT_END + 1)) + (2 << gcdBANK_BIT_START)) -  (BaseAddress & byteMask);

        /* Minimum 256 byte alignment needed for fast_msaa. */
        if ((gcdBANK_CHANNEL_BIT > 7) ||
            ((gckHARDWARE_IsFeatureAvailable(Kernel->hardware, gcvFEATURE_FAST_MSAA) != gcvSTATUS_TRUE) &&
             (gckHARDWARE_IsFeatureAvailable(Kernel->hardware, gcvFEATURE_SMALL_MSAA) != gcvSTATUS_TRUE)))
        {
            /* Add a channel offset at the channel bit. */
            *AlignmentOffset += (1 << gcdBANK_CHANNEL_BIT);
        }
        break;

    default:
        /* no alignment needed. */
        *AlignmentOffset = 0;
    }

    /* Return the status. */
    gcmkFOOTER_ARG("*AlignmentOffset=%u", *AlignmentOffset);
    return gcvSTATUS_OK;
}
#endif

static gcuVIDMEM_NODE_PTR
_FindNode(
    IN gckKERNEL Kernel,
    IN gckVIDMEM Memory,
    IN gctINT Bank,
    IN gctSIZE_T Bytes,
    IN gceVIDMEM_TYPE Type,
    IN OUT gctUINT32_PTR Alignment
    )
{
    gcuVIDMEM_NODE_PTR node;
    gctUINT32 alignment;

#if gcdENABLE_BANK_ALIGNMENT
    gctUINT32 bankAlignment;
    gceSTATUS status;
#endif

    if (Memory->sentinel[Bank].VidMem.nextFree == gcvNULL)
    {
        /* No free nodes left. */
        return gcvNULL;
    }

#if gcdENABLE_BANK_ALIGNMENT
    /* Walk all free nodes until we have one that is big enough or we have
    ** reached the sentinel. */
    for (node = Memory->sentinel[Bank].VidMem.nextFree;
         node->VidMem.bytes != 0;
         node = node->VidMem.nextFree)
    {
        if (node->VidMem.bytes < Bytes)
        {
            continue;
        }

        gcmkONERROR(_GetSurfaceBankAlignment(
            Kernel,
            Type,
            (gctUINT32)(node->VidMem.parent->physicalBase + node->VidMem.offset),
            &bankAlignment));

        bankAlignment = gcmALIGN(bankAlignment, *Alignment);

        /* Compute number of bytes to skip for alignment. */
        alignment = (*Alignment == 0)
                  ? 0
                  : (*Alignment - (node->VidMem.offset % *Alignment));

        if (alignment == *Alignment)
        {
            /* Node is already aligned. */
            alignment = 0;
        }

        if (node->VidMem.bytes >= Bytes + alignment + bankAlignment)
        {
            /* This node is big enough. */
            *Alignment = alignment + bankAlignment;
            return node;
        }
    }
#endif

    /* Walk all free nodes until we have one that is big enough or we have
       reached the sentinel. */
    for (node = Memory->sentinel[Bank].VidMem.nextFree;
         node->VidMem.bytes != 0;
         node = node->VidMem.nextFree)
    {
        gctUINT offset;

        gctINT modulo;

        gcmkSAFECASTSIZET(offset, node->VidMem.offset);

        modulo = gckMATH_ModuloInt(offset, *Alignment);

        /* Compute number of bytes to skip for alignment. */
        alignment = (*Alignment == 0) ? 0 : (*Alignment - modulo);

        if (alignment == *Alignment)
        {
            /* Node is already aligned. */
            alignment = 0;
        }

        if (node->VidMem.bytes >= Bytes + alignment)
        {
            /* This node is big enough. */
            *Alignment = alignment;
            return node;
        }
    }

#if gcdENABLE_BANK_ALIGNMENT
OnError:
#endif
    /* Not enough memory. */
    return gcvNULL;
}

/*******************************************************************************
**
**  gckVIDMEM_AllocateLinear
**
**  Allocate linear memory from the gckVIDMEM object.
**
**  INPUT:
**
**      gckVIDMEM Memory
**          Pointer to an gckVIDMEM object.
**
**      gctSIZE_T Bytes
**          Number of bytes to allocate.
**
**      gctUINT32 Alignment
**          Byte alignment for allocation.
**
**      gceVIDMEM_TYPE Type
**          Type of surface to allocate (use by bank optimization).
**
**      gctBOOL Specified
**          If user must use this pool, it should set Specified to gcvTRUE,
**          otherwise allocator may reserve some memory for other usage, such
**          as small block size allocation request.
**
**  OUTPUT:
**
**      gcuVIDMEM_NODE_PTR * Node
**          Pointer to a variable that will hold the allocated memory node.
*/
static gceSTATUS
gckVIDMEM_AllocateLinear(
    IN gckKERNEL Kernel,
    IN gckVIDMEM Memory,
    IN gctSIZE_T Bytes,
    IN gctUINT32 Alignment,
    IN gceVIDMEM_TYPE Type,
    IN gctBOOL Specified,
    OUT gcuVIDMEM_NODE_PTR * Node
    )
{
    gceSTATUS status;
    gcuVIDMEM_NODE_PTR node;
    gctUINT32 alignment;
    gctINT bank, i;
    gctBOOL acquired = gcvFALSE;

    gcmkHEADER_ARG("Memory=0x%x Bytes=%lu Alignment=%u Type=%d",
                   Memory, Bytes, Alignment, Type);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Memory, gcvOBJ_VIDMEM);
    gcmkVERIFY_ARGUMENT(Bytes > 0);
    gcmkVERIFY_ARGUMENT(Node != gcvNULL);
    gcmkVERIFY_ARGUMENT(Type < gcvVIDMEM_TYPE_COUNT);

    /* Acquire the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(Memory->os, Memory->mutex, gcvINFINITE));

    acquired = gcvTRUE;

    if (Bytes > Memory->freeBytes)
    {
        /* Not enough memory. */
        status = gcvSTATUS_OUT_OF_MEMORY;
        goto OnError;
    }

#if gcdSMALL_BLOCK_SIZE
    if ((Memory->freeBytes < (Memory->bytes/gcdRATIO_FOR_SMALL_MEMORY))
    &&  (Bytes >= gcdSMALL_BLOCK_SIZE)
    &&  (Specified == gcvFALSE)
    )
    {
        /* The left memory is for small memory.*/
        status = gcvSTATUS_OUT_OF_MEMORY;
        goto OnError;
    }
#endif

    /* Find the default bank for this surface type. */
    gcmkASSERT((gctINT) Type < gcmCOUNTOF(Memory->mapping));
    bank      = Memory->mapping[Type];
    alignment = Alignment;

    /* Find a free node in the default bank. */
    node = _FindNode(Kernel, Memory, bank, Bytes, Type, &alignment);

    /* Out of memory? */
    if (node == gcvNULL)
    {
        /* Walk all lower banks. */
        for (i = bank - 1; i >= 0; --i)
        {
            /* Find a free node inside the current bank. */
            node = _FindNode(Kernel, Memory, i, Bytes, Type, &alignment);
            if (node != gcvNULL)
            {
                break;
            }
        }
    }

    if (node == gcvNULL)
    {
        /* Walk all upper banks. */
        for (i = bank + 1; i < gcmCOUNTOF(Memory->sentinel); ++i)
        {
            if (Memory->sentinel[i].VidMem.nextFree == gcvNULL)
            {
                /* Abort when we reach unused banks. */
                break;
            }

            /* Find a free node inside the current bank. */
            node = _FindNode(Kernel, Memory, i, Bytes, Type, &alignment);
            if (node != gcvNULL)
            {
                break;
            }
        }
    }

    if (node == gcvNULL)
    {
        /* Out of memory. */
        status = gcvSTATUS_OUT_OF_MEMORY;
        goto OnError;
    }

    /* Do we have an alignment? */
    if (alignment > 0)
    {
        /* Split the node so it is aligned. */
        if (_Split(Memory->os, node, alignment))
        {
            /* Successful split, move to aligned node. */
            node = node->VidMem.next;

            /* Remove alignment. */
            alignment = 0;
        }
    }

    /* Do we have enough memory after the allocation to split it? */
    if (node->VidMem.bytes - Bytes > Memory->threshold)
    {
        /* Adjust the node size. */
        _Split(Memory->os, node, Bytes);
    }

    /* Remove the node from the free list. */
    node->VidMem.prevFree->VidMem.nextFree = node->VidMem.nextFree;
    node->VidMem.nextFree->VidMem.prevFree = node->VidMem.prevFree;
    node->VidMem.nextFree                  =
    node->VidMem.prevFree                  = gcvNULL;

    /* Fill in the information. */
    node->VidMem.alignment = alignment;
    node->VidMem.parent    = Memory;
#ifdef __QNXNTO__
    node->VidMem.logical   = gcvNULL;
    gcmkONERROR(gckOS_GetProcessID(&node->VidMem.processID));
#endif

    /* Adjust the number of free bytes. */
    Memory->freeBytes   -= node->VidMem.bytes;

    if (Memory->freeBytes < Memory->minFreeBytes)
    {
        Memory->minFreeBytes = Memory->freeBytes;
    }


    /* Release the mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(Memory->os, Memory->mutex));

    /* Return the pointer to the node. */
    *Node = node;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                   "Allocated %u bytes @ 0x%x [0x%08X]",
                   node->VidMem.bytes, node, node->VidMem.offset);

    /* Success. */
    gcmkFOOTER_ARG("*Node=0x%x", *Node);
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
     /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Memory->os, Memory->mutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckVIDMEM_AllocateVirtual
**
**  Construct a new gcuVIDMEM_NODE union for virtual memory.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gctSIZE_T Bytes
**          Number of byte to allocate.
**
**  OUTPUT:
**
**      gcuVIDMEM_NODE_PTR * Node
**          Pointer to a variable that receives the gcuVIDMEM_NODE union pointer.
*/
static gceSTATUS
gckVIDMEM_AllocateVirtual(
    IN gckKERNEL Kernel,
    IN gctUINT32 Flag,
    IN gctSIZE_T Bytes,
    OUT gcuVIDMEM_NODE_PTR * Node
    )
{
    gckOS os;
    gceSTATUS status;
    gcuVIDMEM_NODE_PTR node = gcvNULL;
    gctPOINTER pointer = gcvNULL;
    gctINT i;

    gcmkHEADER_ARG("Kernel=0x%x Flag=%x Bytes=%lu", Kernel, Flag, Bytes);

    /* Verify the arguments. */
    gcmkVERIFY_OBJECT(Kernel, gcvOBJ_KERNEL);
    gcmkVERIFY_ARGUMENT(Bytes > 0);
    gcmkVERIFY_ARGUMENT(Node != gcvNULL);

    /* Extract the gckOS object pointer. */
    os = Kernel->os;
    gcmkVERIFY_OBJECT(os, gcvOBJ_OS);

    /* Allocate an gcuVIDMEM_NODE union. */
    gcmkONERROR(gckOS_Allocate(os, gcmSIZEOF(gcuVIDMEM_NODE), &pointer));

    node = pointer;

    /* Initialize gcuVIDMEM_NODE union for virtual memory. */
    node->Virtual.kernel        = Kernel;
    node->Virtual.contiguous    = Flag & gcvALLOC_FLAG_CONTIGUOUS;
    node->Virtual.logical       = gcvNULL;
    node->Virtual.kvaddr        = gcvNULL;
    node->Virtual.bytes         = Bytes;
    node->Virtual.secure        = (Flag & gcvALLOC_FLAG_SECURITY) != 0;
    node->Virtual.onFault       = (Flag & gcvALLOC_FLAG_ALLOC_ON_FAULT) != 0;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        node->Virtual.lockeds[i]        = 0;
        node->Virtual.pageTables[i]     = gcvNULL;
    }

    /* Allocate the virtual memory. */
    gcmkONERROR(
        gckOS_AllocatePagedMemory(os,
                                  Flag,
                                  &node->Virtual.bytes,
                                  &node->Virtual.gid,
                                  &node->Virtual.physical));

    /* Calculate required GPU page (4096) count. */
    /* Assume start address is 4096 aligned. */
    node->Virtual.pageCount = (node->Virtual.bytes + (4096 - 1)) >> 12;

    /* Return pointer to the gcuVIDMEM_NODE union. */
    *Node = node;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                   "Created virtual node 0x%x for %u bytes @ 0x%x",
                   node, Bytes, node->Virtual.physical);

    /* Success. */
    gcmkFOOTER_ARG("*Node=0x%x", *Node);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    if (node != gcvNULL)
    {
        /* Free the structure. */
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, node));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckVIDMEM_Free
**
**  Free an allocated video memory node.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to a gcuVIDMEM_NODE object.
**
**  OUTPUT:
**
**      Nothing.
*/
static gceSTATUS
gckVIDMEM_Free(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR Node
    )
{
    gceSTATUS status;
    gckKERNEL kernel = gcvNULL;
    gckVIDMEM memory = gcvNULL;
    gcuVIDMEM_NODE_PTR node;
    gctBOOL mutexAcquired = gcvFALSE;

    gcmkHEADER_ARG("Node=0x%x", Node);

    /* Verify the arguments. */
    if ((Node == gcvNULL)
    ||  (Node->VidMem.parent == gcvNULL)
    )
    {
        /* Invalid object. */
        gcmkONERROR(gcvSTATUS_INVALID_OBJECT);
    }

    /**************************** Video Memory ********************************/

    if (Node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        /* Extract pointer to gckVIDMEM object owning the node. */
        memory = Node->VidMem.parent;

        /* Acquire the mutex. */
        gcmkONERROR(
            gckOS_AcquireMutex(memory->os, memory->mutex, gcvINFINITE));

        mutexAcquired = gcvTRUE;

        if (Node->VidMem.kvaddr)
        {
            gcmkONERROR(
                gckOS_DestroyKernelMapping(Kernel->os,
                                           Node->VidMem.parent->physical,
                                           Node->VidMem.kvaddr));

            Node->VidMem.kvaddr = gcvNULL;
        }

#ifdef __QNXNTO__
        /* Unmap the video memory. */
        if (Node->VidMem.logical != gcvNULL)
        {
            gckKERNEL_UnmapVideoMemory(
                Kernel,
                Node->VidMem.logical,
                Node->VidMem.processID,
                Node->VidMem.bytes
                );

            Node->VidMem.logical = gcvNULL;
        }

        /* Reset. */
        Node->VidMem.processID = 0;

        /* Don't try to re-free an already freed node. */
        if ((Node->VidMem.nextFree == gcvNULL)
        &&  (Node->VidMem.prevFree == gcvNULL)
        )
#endif
        {

            /* Check if Node is already freed. */
            if (Node->VidMem.nextFree)
            {
                /* Node is alread freed. */
                gcmkONERROR(gcvSTATUS_INVALID_DATA);
            }

            /* Update the number of free bytes. */
            memory->freeBytes += Node->VidMem.bytes;

            /* Find the next free node. */
            for (node = Node->VidMem.next;
                 node != gcvNULL && node->VidMem.nextFree == gcvNULL;
                 node = node->VidMem.next) ;

            /* Insert this node in the free list. */
            Node->VidMem.nextFree = node;
            Node->VidMem.prevFree = node->VidMem.prevFree;

            Node->VidMem.prevFree->VidMem.nextFree =
            node->VidMem.prevFree                  = Node;

            /* Is the next node a free node and not the sentinel? */
            if ((Node->VidMem.next == Node->VidMem.nextFree)
            &&  (Node->VidMem.next->VidMem.bytes != 0)
            )
            {
                /* Merge this node with the next node. */
                gcmkONERROR(_Merge(memory->os, node = Node));
                gcmkASSERT(node->VidMem.nextFree != node);
                gcmkASSERT(node->VidMem.prevFree != node);
            }

            /* Is the previous node a free node and not the sentinel? */
            if ((Node->VidMem.prev == Node->VidMem.prevFree)
            &&  (Node->VidMem.prev->VidMem.bytes != 0)
            )
            {
                /* Merge this node with the previous node. */
                gcmkONERROR(_Merge(memory->os, node = Node->VidMem.prev));
                gcmkASSERT(node->VidMem.nextFree != node);
                gcmkASSERT(node->VidMem.prevFree != node);
            }
        }

        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(memory->os, memory->mutex));

        gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                       "Node 0x%x is freed.",
                       Node);

        /* Success. */
        gcmkFOOTER_NO();
        return gcvSTATUS_OK;
    }

    /*************************** Virtual Memory *******************************/

    /* Get gckKERNEL object. */
    kernel = Node->Virtual.kernel;

    /* Verify the gckKERNEL object pointer. */
    gcmkVERIFY_OBJECT(kernel, gcvOBJ_KERNEL);


    if (Node->Virtual.kvaddr)
    {
        gcmkVERIFY_OK(
            gckOS_DestroyKernelMapping(kernel->os,
                                       Node->Virtual.physical,
                                       Node->Virtual.kvaddr));
    }

    /* Free the virtual memory. */
    gcmkVERIFY_OK(gckOS_FreePagedMemory(kernel->os,
                                        Node->Virtual.physical,
                                        Node->Virtual.bytes));

    /* Delete the gcuVIDMEM_NODE union. */
    gcmkVERIFY_OK(gcmkOS_SAFE_FREE(kernel->os, Node));

    /* Success. */
    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (mutexAcquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(
            memory->os, memory->mutex
            ));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

#if !gcdPROCESS_ADDRESS_SPACE
/*******************************************************************************
**
** _ConvertPhysical
**
**  Convert CPU physical to GPU address for video node.
**
**  INPUT:
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to a gcuVIDMEM_NODE union.
**
**      gceCORE Core
**          Id of current GPU.
**
**      gctPHYS_ADDR_T PhysicalAddress
**          CPU physical address
**
**  OUTPUT:
**      gctUINT32 * Address
**          A pointer hold the GPU address.
*/
static gceSTATUS
_ConvertPhysical(
    IN gckKERNEL Kernel,
    IN gceCORE Core,
    IN gcuVIDMEM_NODE_PTR Node,
    IN gctPHYS_ADDR_T PhysicalAddress,
    OUT gctUINT32 * Address
    )
{
    gceSTATUS status;
    gctUINT64 physical = 0;

    gcmkHEADER_ARG("Node=0x%X", Node);

    if (!Node->Virtual.contiguous)
    {
        /* non-contiguous, mapping is required. */
        status = gcvSTATUS_NOT_SUPPORTED;
        goto OnError;
    }

    if (Node->Virtual.secure)
    {
        /* Secure, mapping is forced. */
        status = gcvSTATUS_NOT_SUPPORTED;
        goto OnError;
    }

    /* Convert to GPU physical address. */
    gckOS_CPUPhysicalToGPUPhysical(Kernel->os, PhysicalAddress, &physical);


    if ((physical > gcvMAXUINT32) ||
        (physical + Node->Virtual.bytes - 1 > gcvMAXUINT32))
    {
        /* Above 4G (32bit), mapping is required currently. */
        status = gcvSTATUS_NOT_SUPPORTED;
        goto OnError;
    }

    if (!gckHARDWARE_IsFeatureAvailable(Kernel->hardware, gcvFEATURE_MMU))
    {
        if (physical < Kernel->hardware->baseAddress)
        {
            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        /* Subtract baseAddress to get a GPU address used for programming. */
        physical -= Kernel->hardware->baseAddress;

        /* 2G upper is virtual space, better to move to gckHARDWARE section. */
        if (physical + Node->Virtual.bytes > 0x80000000)
        {
            /* End is above 2G, ie virtual space. */
            status = gcvSTATUS_NOT_SUPPORTED;
            goto OnError;
        }

        *Address = (gctUINT32)physical;

        gcmkFOOTER_ARG("*Address=0x%X", *Address);
        return gcvSTATUS_OK;
    }
    else
    {
        gctBOOL flatMapped;

        gcmkONERROR(gckMMU_IsFlatMapped(Kernel->mmu, (gctUINT32)physical, &flatMapped));

        if (!flatMapped)
        {
            status = gcvSTATUS_NOT_SUPPORTED;
            goto OnError;
        }

        *Address = (gctUINT32)physical;

        gcmkFOOTER_ARG("*Address=0x%X", *Address);
        return gcvSTATUS_OK;
    }

OnError:
    gcmkFOOTER();
    return status;
}
#endif

#if gcdPROCESS_ADDRESS_SPACE
gcsGPU_MAP_PTR
_FindGPUMap(
    IN gcsGPU_MAP_PTR Head,
    IN gctINT ProcessID
    )
{
    gcsGPU_MAP_PTR map = Head;

    while (map)
    {
        if (map->pid == ProcessID)
        {
            return map;
        }

        map = map->next;
    }

    return gcvNULL;
}

gcsGPU_MAP_PTR
_CreateGPUMap(
    IN gckOS Os,
    IN gcsGPU_MAP_PTR *Head,
    IN gcsGPU_MAP_PTR *Tail,
    IN gctINT ProcessID
    )
{
    gcsGPU_MAP_PTR gpuMap;
    gctPOINTER pointer = gcvNULL;

    gckOS_Allocate(Os, sizeof(gcsGPU_MAP), &pointer);

    if (pointer == gcvNULL)
    {
        return gcvNULL;
    }

    gpuMap = pointer;

    gckOS_ZeroMemory(pointer, sizeof(gcsGPU_MAP));

    gpuMap->pid = ProcessID;

    if (!*Head)
    {
        *Head = *Tail = gpuMap;
    }
    else
    {
        gpuMap->prev = *Tail;
        (*Tail)->next = gpuMap;
        *Tail = gpuMap;
    }

    return gpuMap;
}

void
_DestroyGPUMap(
    IN gckOS Os,
    IN gcsGPU_MAP_PTR *Head,
    IN gcsGPU_MAP_PTR *Tail,
    IN gcsGPU_MAP_PTR gpuMap
    )
{

    if (gpuMap == *Head)
    {
        if ((*Head = gpuMap->next) == gcvNULL)
        {
            *Tail = gcvNULL;
        }
    }
    else
    {
        gpuMap->prev->next = gpuMap->next;
        if (gpuMap == *Tail)
        {
            *Tail = gpuMap->prev;
        }
        else
        {
            gpuMap->next->prev = gpuMap->prev;
        }
    }

    gcmkOS_SAFE_FREE(Os, gpuMap);
}
#endif

/*******************************************************************************
**
**  gckVIDMEM_Lock
**
**  Lock a video memory node and return its hardware specific address.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to a gcuVIDMEM_NODE union.
**
**  OUTPUT:
**
**      gctUINT32 * Address
**          Pointer to a variable that will hold the hardware specific address.
**
**      gctUINT32 * PhysicalAddress
**          Pointer to a variable that will hold the bus address of a contiguous
**          video node.
*/
static gceSTATUS
gckVIDMEM_Lock(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR Node,
    OUT gctUINT32 * Address
    )
{
    gckOS os;

    gcmkHEADER_ARG("Kernel=%p Node=%p", Kernel, Node);

    /* Extract the gckOS object pointer. */
    os = Kernel->os;

    /* Increment the lock count. */
    if (Node->VidMem.locked++ == 0)
    {
        gctUINT32 address;
        gctUINT32 offset = (gctUINT32)Node->VidMem.offset;

        switch (Node->VidMem.pool)
        {
        case gcvPOOL_LOCAL_EXTERNAL:
            address = Kernel->externalBaseAddress + offset;
            break;
        case gcvPOOL_LOCAL_INTERNAL:
            address = Kernel->internalBaseAddress + offset;
            break;
        default:
            gcmkASSERT(Node->VidMem.pool == gcvPOOL_SYSTEM);
        case gcvPOOL_SYSTEM:
            address = Kernel->contiguousBaseAddress + offset;
            break;
        }

        /* Save address. */
        Node->VidMem.address = address;
    }

    *Address = Node->VidMem.address;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "Locked node 0x%x (%d) @ 0x%08X",
                  Node,
                  Node->VidMem.locked,
                  *Address);

    gcmkFOOTER_ARG("*Address=0x%08X", *Address);
    return gcvSTATUS_OK;
}

static gceSTATUS
gckVIDMEM_LockVirtual(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR Node,
    OUT gctUINT32 * Address
    )
{
    gceSTATUS status;
    gctPHYS_ADDR_T physicalAddress;
    gctBOOL locked = gcvFALSE;
    gckOS os = Kernel->os;

    gcmkHEADER_ARG("Kernel=%p Node=%p", Kernel, Node);

    gcmkONERROR(
        gckOS_GetPhysicalFromHandle(os,
                                    Node->Virtual.physical,
                                    0,
                                    &physicalAddress));

    /* Expect 4096 aligned. */
    gcmkASSERT((physicalAddress & 0xFFF) == 0);


#if !gcdPROCESS_ADDRESS_SPACE
    /* Increment the lock count. */
    if (Node->Virtual.lockeds[Kernel->core]++ == 0)
    {
        locked = gcvTRUE;

        status = _ConvertPhysical(
            Kernel,
            Kernel->core,
            Node,
            physicalAddress,
            &Node->Virtual.addresses[Kernel->core]
            );

        if (gcmIS_ERROR(status))
        {
            /* Do GPU address mapping. */
#if gcdSECURITY
            gctPHYS_ADDR physicalArrayPhysical;
            gctPOINTER physicalArrayLogical;

            gcmkONERROR(gckOS_AllocatePageArray(
                os,
                Node->Virtual.physical,
                Node->Virtual.pageCount,
                &physicalArrayLogical,
                &physicalArrayPhysical
                ));

            gcmkONERROR(gckKERNEL_SecurityMapMemory(
                Kernel,
                physicalArrayLogical,
                Node->Virtual.pageCount,
                &Node->Virtual.addresses[Kernel->core]
                ));

            gcmkONERROR(gckOS_FreeNonPagedMemory(
                os,
                physicalArrayPhysical,
                physicalArrayLogical,
                1
                ));
#else
            {
                /* Allocate pages inside the MMU. */
                gcmkONERROR(
                    gckMMU_AllocatePagesEx(Kernel->mmu,
                                           Node->Virtual.pageCount,
                                           Node->Virtual.type,
                                           Node->Virtual.secure,
                                           &Node->Virtual.pageTables[Kernel->core],
                                           &Node->Virtual.addresses[Kernel->core]));
            }

            if (Node->Virtual.onFault != gcvTRUE)
            {
#if gcdENABLE_TRUST_APPLICATION
                if (Kernel->hardware->options.secureMode == gcvSECURE_IN_TA)
                {
                    gcmkONERROR(gckKERNEL_MapInTrustApplicaiton(
                        Kernel,
                        Node->Virtual.logical,
                        Node->Virtual.physical,
                        Node->Virtual.addresses[Kernel->core],
                        Node->Virtual.pageCount
                        ));
                }
                else
#endif
                {
                    /* Map the pages. */
                    gcmkONERROR(gckOS_MapPagesEx(os,
                        Kernel->core,
                        Node->Virtual.physical,
                        Node->Virtual.pageCount,
                        Node->Virtual.addresses[Kernel->core],
                        Node->Virtual.pageTables[Kernel->core],
                        gcvTRUE,
                        Node->Virtual.type));
                }
            }

            {
                gcmkONERROR(gckMMU_Flush(Kernel->mmu, Node->Virtual.type));
            }
#endif

            /* GPU MMU page size is fixed at 4096 now. */
            Node->Virtual.addresses[Kernel->core] |= (gctUINT32)physicalAddress & (4096 - 1);
        }

        gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                       "Mapped virtual node 0x%x to 0x%08X",
                       Node,
                       Node->Virtual.addresses[Kernel->core]);
    }
#endif

    /* Return hardware address. */
    *Address = Node->Virtual.addresses[Kernel->core];

    gcmkFOOTER_ARG("*Address=0x%08X", *Address);
    return gcvSTATUS_OK;

OnError:
    if (locked)
    {
        if (Node->Virtual.pageTables[Kernel->core] != gcvNULL)
        {
            {
                /* Free the pages from the MMU. */
                gcmkVERIFY_OK(
                    gckMMU_FreePages(Kernel->mmu,
                                     Node->Virtual.secure,
                                     Node->Virtual.addresses[Kernel->core],
                                     Node->Virtual.pageTables[Kernel->core],
                                     Node->Virtual.pageCount));
            }

            Node->Virtual.pageTables[Kernel->core] = gcvNULL;
        }

        Node->Virtual.lockeds[Kernel->core]--;
    }

    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckVIDMEM_Unlock
**
**  Unlock a video memory node.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gcuVIDMEM_NODE_PTR Node
**          Pointer to a locked gcuVIDMEM_NODE union.
**
**      gctBOOL * Asynchroneous
**          Pointer to a variable specifying whether the surface should be
**          unlocked asynchroneously or not.
**
**  OUTPUT:
**
**      gctBOOL * Asynchroneous
**          Pointer to a variable receiving the number of bytes used in the
**          command buffer specified by 'Commands'.  If gcvNULL, there is no
**          command buffer.
*/
static gceSTATUS
gckVIDMEM_Unlock(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR Node,
    IN OUT gctBOOL * Asynchroneous
    )
{
    gceSTATUS status;

    gcmkHEADER_ARG("Node=0x%x *Asynchroneous=%d",
                   Node, gcmOPT_VALUE(Asynchroneous));

    if (Node->VidMem.locked <= 0)
    {
        /* The surface was not locked. */
        gcmkONERROR(gcvSTATUS_MEMORY_UNLOCKED);
    }

    if (Asynchroneous != gcvNULL)
    {
        /* Schedule an event to sync with GPU. */
        *Asynchroneous = gcvTRUE;
    }
    else
    {
        /* Decrement the lock count. */
        Node->VidMem.locked--;
    }

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                  "Unlocked node %p (%d)",
                  Node,
                  Node->VidMem.locked);

    /* Success. */
    gcmkFOOTER_ARG("*Asynchroneous=%d", gcmOPT_VALUE(Asynchroneous));
    return gcvSTATUS_OK;

OnError:
    /* Return the status. */
    gcmkFOOTER();
    return status;
}

static gceSTATUS
gckVIDMEM_UnlockVirtual(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR Node,
    IN OUT gctBOOL * Asynchroneous
    )
{
    gceSTATUS status;

    gcmkHEADER_ARG("Node=0x%x *Asynchroneous=%d",
                   Node, gcmOPT_VALUE(Asynchroneous));

    if (Asynchroneous != gcvNULL)
    {
        /* Schedule the surface to be unlocked. */
        *Asynchroneous = gcvTRUE;
    }
    else
    {
#if !gcdPROCESS_ADDRESS_SPACE
        if (Node->Virtual.lockeds[Kernel->core] == 0)
        {
            gcmkONERROR(gcvSTATUS_MEMORY_UNLOCKED);
        }

        /* Decrement lock count. */
        --Node->Virtual.lockeds[Kernel->core];

        /* See if we can unlock the resources. */
        if (Node->Virtual.lockeds[Kernel->core] == 0)
        {
            gctUINT32 address;

            /* Adjust address to page aligned for underlying functions. */
            address = Node->Virtual.addresses[Kernel->core] & (4096 - 1);

#if gcdSECURITY
            if (Node->Virtual.addresses[Kernel->core] > 0x80000000)
            {
                gcmkONERROR(gckKERNEL_SecurityUnmapMemory(
                    Kernel,
                    address,
                    Node->Virtual.pageCount
                    ));
            }
#else
            /* Free the page table. */
            if (Node->Virtual.pageTables[Kernel->core] != gcvNULL)
            {
                {
                    gcmkONERROR(
                        gckMMU_FreePages(Kernel->mmu,
                                         Node->Virtual.secure,
                                         address,
                                         Node->Virtual.pageTables[Kernel->core],
                                         Node->Virtual.pageCount));
                }

                gcmkONERROR(gckOS_UnmapPages(
                    Kernel->os,
                    Node->Virtual.pageCount,
                    address
                    ));

                /* Mark page table as freed. */
                Node->Virtual.pageTables[Kernel->core] = gcvNULL;
            }
#endif
        }

        gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_VIDMEM,
                       "Unmapped virtual node %p from 0x%08X",
                       Node, Node->Virtual.addresses[Kernel->core]);
#endif
    }

    /* Success. */
    gcmkFOOTER_ARG("*Asynchroneous=%d", gcmOPT_VALUE(Asynchroneous));
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}


/*******************************************************************************
**
**  gckVIDMEM_HANDLE_Allocate
**
**  Allocate a handle for a gckVIDMEM_NODE object.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Pointer to an gckKERNEL object.
**
**      gckVIDMEM_NODE Node
**          Pointer to a gckVIDMEM_NODE object.
**
**  OUTPUT:
**
**      gctUINT32 * Handle
**          Pointer to a variable receiving a handle represent this
**          gckVIDMEM_NODE in userspace.
*/
gceSTATUS
gckVIDMEM_HANDLE_Allocate(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE Node,
    OUT gctUINT32 * Handle
    )
{
    gceSTATUS status;
    gctUINT32 processID           = 0;
    gctPOINTER pointer            = gcvNULL;
    gctPOINTER handleDatabase     = gcvNULL;
    gctPOINTER mutex              = gcvNULL;
    gctUINT32 handle              = 0;
    gckVIDMEM_HANDLE handleObject = gcvNULL;
    gckOS os                      = Kernel->os;

    gcmkHEADER_ARG("Kernel=0x%X, Node=0x%X", Kernel, Node);

    gcmkVERIFY_OBJECT(os, gcvOBJ_OS);

    /* Allocate a gckVIDMEM_HANDLE object. */
    gcmkONERROR(gckOS_Allocate(os, gcmSIZEOF(gcsVIDMEM_HANDLE), &pointer));

    gcmkVERIFY_OK(gckOS_ZeroMemory(pointer, gcmSIZEOF(gcsVIDMEM_HANDLE)));

    handleObject = pointer;

    gcmkONERROR(gckOS_AtomConstruct(os, &handleObject->reference));

    /* Set default reference count to 1. */
    gckOS_AtomSet(os, handleObject->reference, 1);

    gcmkVERIFY_OK(gckOS_GetProcessID(&processID));

    gcmkONERROR(
        gckKERNEL_FindHandleDatbase(Kernel,
                                    processID,
                                    &handleDatabase,
                                    &mutex));

    /* Allocate a handle for this object. */
    gcmkONERROR(
        gckKERNEL_AllocateIntegerId(handleDatabase, handleObject, &handle));

    handleObject->node = Node;
    handleObject->handle = handle;

    *Handle = handle;

    gcmkFOOTER_ARG("*Handle=%d", *Handle);
    return gcvSTATUS_OK;

OnError:
    if (handleObject != gcvNULL)
    {
        if (handleObject->reference != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_AtomDestroy(os, handleObject->reference));
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, handleObject));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_HANDLE_Reference(
    IN gckKERNEL Kernel,
    IN gctUINT32 ProcessID,
    IN gctUINT32 Handle
    )
{
    gceSTATUS status;
    gckVIDMEM_HANDLE handleObject = gcvNULL;
    gctPOINTER database           = gcvNULL;
    gctPOINTER mutex              = gcvNULL;
    gctINT32 oldValue             = 0;
    gctBOOL acquired              = gcvFALSE;

    gcmkHEADER_ARG("Handle=%d PrcoessID=%d", Handle, ProcessID);

    gcmkONERROR(
        gckKERNEL_FindHandleDatbase(Kernel, ProcessID, &database, &mutex));

    gcmkVERIFY_OK(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Translate handle to gckVIDMEM_HANDLE object. */
    gcmkONERROR(
        gckKERNEL_QueryIntegerId(database, Handle, (gctPOINTER *)&handleObject));

    /* Increase the reference count. */
    gckOS_AtomIncrement(Kernel->os, handleObject->reference, &oldValue);

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_HANDLE_Dereference(
    IN gckKERNEL Kernel,
    IN gctUINT32 ProcessID,
    IN gctUINT32 Handle
    )
{
    gceSTATUS status;
    gctPOINTER handleDatabase     = gcvNULL;
    gctPOINTER mutex              = gcvNULL;
    gctINT32 oldValue             = 0;
    gckVIDMEM_HANDLE handleObject = gcvNULL;
    gctBOOL acquired              = gcvFALSE;

    gcmkHEADER_ARG("Handle=%d PrcoessID=%d", Handle, ProcessID);

    gcmkONERROR(
        gckKERNEL_FindHandleDatbase(Kernel,
                                    ProcessID,
                                    &handleDatabase,
                                    &mutex));

    gcmkVERIFY_OK(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Translate handle to gckVIDMEM_HANDLE. */
    gcmkONERROR(
        gckKERNEL_QueryIntegerId(handleDatabase, Handle, (gctPOINTER *)&handleObject));

    gckOS_AtomDecrement(Kernel->os, handleObject->reference, &oldValue);

    if (oldValue == 1)
    {
        /* Remove handle from database if this is the last reference. */
        gcmkVERIFY_OK(gckKERNEL_FreeIntegerId(handleDatabase, Handle));
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    if (oldValue == 1)
    {
        gcmkVERIFY_OK(gckOS_AtomDestroy(Kernel->os, handleObject->reference));
        gcmkOS_SAFE_FREE(Kernel->os, handleObject);
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_HANDLE_Lookup(
    IN gckKERNEL Kernel,
    IN gctUINT32 ProcessID,
    IN gctUINT32 Handle,
    OUT gckVIDMEM_NODE * Node
    )
{
    gceSTATUS status;
    gckVIDMEM_HANDLE handleObject = gcvNULL;
    gckVIDMEM_NODE node           = gcvNULL;
    gctPOINTER database           = gcvNULL;
    gctPOINTER mutex              = gcvNULL;
    gctBOOL acquired              = gcvFALSE;

    gcmkHEADER_ARG("Kernel=0x%X ProcessID=%d Handle=%d",
                   Kernel, ProcessID, Handle);

    gcmkONERROR(
        gckKERNEL_FindHandleDatbase(Kernel, ProcessID, &database, &mutex));

    gcmkVERIFY_OK(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    gcmkONERROR(
        gckKERNEL_QueryIntegerId(database, Handle, (gctPOINTER *)&handleObject));

    node = handleObject->node;

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    *Node = node;

    gcmkFOOTER_ARG("*Node=%d", *Node);
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_HANDLE_Lookup2(
    IN gckKERNEL Kernel,
    IN gcsDATABASE_PTR Database,
    IN gctUINT32 Handle,
    OUT gckVIDMEM_NODE * Node
    )
{
    gceSTATUS status;
    gckVIDMEM_HANDLE handleObject = gcvNULL;
    gckVIDMEM_NODE node           = gcvNULL;
    gctPOINTER database           = gcvNULL;
    gctPOINTER mutex              = gcvNULL;
    gctBOOL acquired              = gcvFALSE;

    gcmkHEADER_ARG("Kernel=0x%X Database=%p Handle=%d",
                   Kernel, Database, Handle);

    database = Database->handleDatabase;
    mutex = Database->handleDatabaseMutex;

    gcmkVERIFY_OK(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    gcmkONERROR(
        gckKERNEL_QueryIntegerId(database, Handle, (gctPOINTER *)&handleObject));

    node = handleObject->node;

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    *Node = node;

    gcmkFOOTER_ARG("*Node=%d", *Node);
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}


static gceSTATUS
gckVIDMEM_NODE_Construct(
    IN gckKERNEL Kernel,
    IN gcuVIDMEM_NODE_PTR VideoNode,
    IN gceVIDMEM_TYPE Type,
    IN gcePOOL Pool,
    OUT gckVIDMEM_NODE * NodeObject
    )
{
    gceSTATUS status;
    gckVIDMEM_NODE node = gcvNULL;
    gctPOINTER pointer  = gcvNULL;
    gckOS os = Kernel->os;
    gctUINT i;

    /* Construct a node. */
    gcmkONERROR(gckOS_Allocate(os, gcmSIZEOF(gcsVIDMEM_NODE), &pointer));

    gcmkVERIFY_OK(gckOS_ZeroMemory(pointer, gcmSIZEOF(gcsVIDMEM_NODE)));

    node = pointer;

    node->node = VideoNode;
    node->kernel = Kernel;
    node->type = Type;
    node->pool = Pool;

#if gcdPROCESS_ADDRESS_SPACE
    gcmkONERROR(gckOS_CreateMutex(os, &node->mapMutex));
#endif

    gcmkONERROR(gckOS_AtomConstruct(os, &node->reference));

    gcmkONERROR(gckOS_CreateMutex(os, &node->mutex));

    for (i = 0; i < gcvENGINE_GPU_ENGINE_COUNT; i++)
    {
        gcmkONERROR(gckOS_CreateSignal(os, gcvFALSE, &node->sync[i].signal));
    }

    /* Reference is 1 by default . */
    gckOS_AtomSet(os, node->reference, 1);

    gcmkVERIFY_OK(
        gckOS_AcquireMutex(Kernel->os,
                           Kernel->db->videoMemListMutex,
                           gcvINFINITE));

    /* Add into video memory node list. */
    gcsLIST_Add(&node->link, &Kernel->db->videoMemList);

    gcmkVERIFY_OK(
        gckOS_ReleaseMutex(Kernel->os, Kernel->db->videoMemListMutex));

    *NodeObject = node;

    return gcvSTATUS_OK;

OnError:
    if (node != gcvNULL)
    {
#if gcdPROCESS_ADDRESS_SPACE
        if (node->mapMutex != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_DeleteMutex(os, node->mapMutex));
        }
#endif

        if (node->mutex)
        {
            gcmkVERIFY_OK(gckOS_DeleteMutex(os, node->mutex));
        }

        if (node->reference != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_AtomDestroy(os, node->reference));
        }

        for (i = 0; i < gcvENGINE_GPU_ENGINE_COUNT; i++)
        {
            if (node->sync[i].signal != gcvNULL)
            {
                gcmkVERIFY_OK(gckOS_DestroySignal(os, node->sync[i].signal));
            }
        }

        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, node));
    }

    return status;
}

gceSTATUS
gckVIDMEM_NODE_AllocateLinear(
    IN gckKERNEL Kernel,
    IN gckVIDMEM VideoMemory,
    IN gcePOOL Pool,
    IN gceVIDMEM_TYPE Type,
    IN gctUINT32 Alignment,
    IN gctBOOL Specified,
    IN OUT gctSIZE_T * Bytes,
    OUT gckVIDMEM_NODE * NodeObject
    )
{
    gceSTATUS status;
    gctSIZE_T bytes = *Bytes;
    gcuVIDMEM_NODE_PTR node = gcvNULL;
    gckVIDMEM_NODE nodeObject = gcvNULL;

    gcmkHEADER_ARG("Kernel=%p VideoMemory=%p Pool=%d Alignment=%d Type=%d *Bytes=%zu",
                   Kernel, VideoMemory, Pool, Alignment, Type, bytes);

    gcmkONERROR(
        gckVIDMEM_AllocateLinear(Kernel,
                                 VideoMemory,
                                 bytes,
                                 Alignment,
                                 Type,
                                 Specified,
                                 &node));

    /* Update pool. */
    node->VidMem.pool = Pool;
    bytes = node->VidMem.bytes;

    /* Construct a node. */
    gcmkONERROR(
        gckVIDMEM_NODE_Construct(Kernel, node, Type, Pool, &nodeObject));

    *Bytes = bytes;
    *NodeObject = nodeObject;

    gcmkFOOTER_ARG("*Bytes=%zu *NodeObject=%p", bytes, nodeObject);
    return gcvSTATUS_OK;

OnError:
    if (node)
    {
        gcmkVERIFY_OK(gckVIDMEM_Free(Kernel, node));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_AllocateVirtual(
    IN gckKERNEL Kernel,
    IN gcePOOL Pool,
    IN gceVIDMEM_TYPE Type,
    IN gctUINT32 Flag,
    IN OUT gctSIZE_T * Bytes,
    OUT gckVIDMEM_NODE * NodeObject
    )
{
    gceSTATUS status;
    gctSIZE_T bytes = *Bytes;
    gcuVIDMEM_NODE_PTR node = gcvNULL;
    gckVIDMEM_NODE nodeObject = gcvNULL;

    gcmkHEADER_ARG("Kernel=%p Pool=%d Type=%d Flag=%x *Bytes=%zu",
                   Kernel, Pool, Type, Flag, bytes);

    gcmkONERROR(
        gckVIDMEM_AllocateVirtual(Kernel, Flag, bytes, &node));

    /* Update type. */
    node->Virtual.type = Type;
    bytes = node->Virtual.bytes;

    /* Construct a node. */
    gcmkONERROR(
        gckVIDMEM_NODE_Construct(Kernel, node, Type, Pool, &nodeObject));

    *Bytes = bytes;
    *NodeObject = nodeObject;

    gcmkFOOTER_ARG("*Bytes=%zu *NodeObject=%p", bytes, nodeObject);
    return gcvSTATUS_OK;

OnError:
    if (node)
    {
        gcmkVERIFY_OK(gckVIDMEM_Free(Kernel, node));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_Reference(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject
    )
{
    gctINT32 oldValue;
    gcmkHEADER_ARG("Kernel=0x%X NodeObject=0x%X", Kernel, NodeObject);

    gckOS_AtomIncrement(Kernel->os, NodeObject->reference, &oldValue);

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_Dereference(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject
    )
{
    gctINT32 oldValue   = 0;
    gctPOINTER database = Kernel->db->nameDatabase;
    gctPOINTER mutex    = Kernel->db->nameDatabaseMutex;
    gctUINT i;

    gcmkHEADER_ARG("Kernel=0x%X NodeObject=0x%X", Kernel, NodeObject);

    gcmkVERIFY_OK(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));

    gcmkVERIFY_OK(gckOS_AtomDecrement(Kernel->os, NodeObject->reference, &oldValue));

    if (oldValue == 1 && NodeObject->name)
    {
        /* Free name if exists. */
        gcmkVERIFY_OK(gckKERNEL_FreeIntegerId(database, NodeObject->name));
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));

    if (oldValue == 1)
    {
        gcmkVERIFY_OK(
            gckOS_AcquireMutex(Kernel->os,
                               Kernel->db->videoMemListMutex,
                               gcvINFINITE));

        /* Remove from video memory node list. */
        gcsLIST_Del(&NodeObject->link);

        gcmkVERIFY_OK(
            gckOS_ReleaseMutex(Kernel->os, Kernel->db->videoMemListMutex));

        /* Free gcuVIDMEM_NODE. */
        gcmkVERIFY_OK(gckVIDMEM_Free(Kernel, NodeObject->node));

        gcmkVERIFY_OK(gckOS_AtomDestroy(Kernel->os, NodeObject->reference));

#if gcdPROCESS_ADDRESS_SPACE
        gcmkVERIFY_OK(gckOS_DeleteMutex(Kernel->os, NodeObject->mapMutex));
#endif

        gcmkVERIFY_OK(gckOS_DeleteMutex(Kernel->os, NodeObject->mutex));

        for (i = 0; i < gcvENGINE_GPU_ENGINE_COUNT; i++)
        {
            if (NodeObject->sync[i].signal != gcvNULL)
            {
                gcmkVERIFY_OK(gckOS_DestroySignal(Kernel->os, NodeObject->sync[i].signal));
            }
        }

        /* Should not cause recursive call since tsNode->tsNode should be NULL */
        if (NodeObject->tsNode)
        {
            gcmkASSERT(!NodeObject->tsNode->tsNode);
            gckVIDMEM_NODE_Dereference(Kernel, NodeObject->tsNode);
        }

        gcmkOS_SAFE_FREE(Kernel->os, NodeObject);
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_GetReference(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctINT32 * ReferenceCount
    )
{
    gctINT32 value;

    gckOS_AtomGet(Kernel->os, NodeObject->reference, &value);

    *ReferenceCount = value;
    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_Lock(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctUINT32 * Address
    )
{
    gceSTATUS status;
    gckOS os = Kernel->os;
    gctBOOL acquired = gcvFALSE;
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    gcmkHEADER_ARG("NodeObject=%p", NodeObject);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(os, NodeObject->mutex, gcvINFINITE));
    acquired = gcvTRUE;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        gcmkONERROR(gckVIDMEM_Lock(Kernel, node, Address));
    }
    else
    {
        gcmkONERROR(gckVIDMEM_LockVirtual(Kernel, node, Address));
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));

    gcmkFOOTER_ARG("*Address=0x%08X", *Address);
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_Unlock(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctUINT32 ProcessID,
    IN OUT gctBOOL * Asynchroneous
    )
{
    gceSTATUS status;
    gckOS os = Kernel->os;
    gctBOOL acquired = gcvFALSE;
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    gcmkHEADER_ARG("NodeObject=%p Asynchroneous=%p", NodeObject, Asynchroneous);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(os, NodeObject->mutex, gcvINFINITE));
    acquired = gcvTRUE;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        gcmkONERROR(gckVIDMEM_Unlock(Kernel, node, Asynchroneous));
    }
    else
    {
        gcmkONERROR(gckVIDMEM_UnlockVirtual(Kernel, node, Asynchroneous));
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));

    gcmkFOOTER_ARG("*Asynchroneous=0x%08X", gcmOPT_VALUE(Asynchroneous));
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_FlushCache(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctSIZE_T Offset,
    IN gctPOINTER Logical,
    IN gctSIZE_T Bytes
    )
{
    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_GetLockCount(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctINT32 * LockCount
    )
{
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        *LockCount = node->VidMem.locked;
    }
    else
    {
        gctUINT i;
        gctINT32 lockCount = 0;

        for (i = 0; i < gcvCORE_COUNT; i++)
        {
            lockCount += node->Virtual.lockeds[i];
        }

        *LockCount = lockCount;
    }

    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_LockCPU(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctBOOL Cacheable,
    IN gctBOOL FromUser,
    OUT gctPOINTER * Logical
    )
{
    gceSTATUS status;
    gckOS os = Kernel->os;
    gctBOOL acquired = gcvFALSE;
    gcuVIDMEM_NODE_PTR node = NodeObject->node;
    gctPOINTER logical = gcvNULL;

    gcmkHEADER_ARG("NodeObject=%p", NodeObject);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(os, NodeObject->mutex, gcvINFINITE));
    acquired = gcvTRUE;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        if (Cacheable == gcvTRUE)
        {
            gcmkONERROR(gcvSTATUS_INVALID_REQUEST);
        }

        if (FromUser)
        {
            /* Map video memory pool to user space. */
#ifdef __QNXNTO__
            if (node->VidMem.logical == gcvNULL)
            {
                gcmkONERROR(
                    gckKERNEL_MapVideoMemory(Kernel,
                                             gcvTRUE,
                                             node->VidMem.pool,
                                             (gctUINT32)node->VidMem.offset,
                                             (gctUINT32)node->VidMem.bytes,
                                             &node->VidMem.logical));
            }

            logical = node->VidMem.logical;
#else
            gcmkONERROR(
                gckKERNEL_MapVideoMemory(Kernel,
                                         gcvTRUE,
                                         node->VidMem.pool,
                                         (gctUINT32)node->VidMem.offset,
                                         (gctUINT32)node->VidMem.bytes,
                                         &logical));
#endif
        }
        else
        {
            /* Map video memory pool to kernel space. */
            if (!node->VidMem.kvaddr)
            {
                gcmkONERROR(
                    gckOS_CreateKernelMapping(os,
                                              node->VidMem.parent->physical,
                                              node->VidMem.offset,
                                              node->VidMem.bytes,
                                              &node->VidMem.kvaddr));
            }

            logical = node->VidMem.kvaddr;
        }
    }
    else
    {
        if (FromUser)
        {
            gcmkONERROR(
                gckOS_LockPages(os,
                                node->Virtual.physical,
                                node->Virtual.bytes,
                                Cacheable,
                                &logical));

            node->Virtual.logical = logical;
        }
        else
        {
            /* Map once and will cancel map when free. */
            if (!node->Virtual.kvaddr)
            {
                gcmkONERROR(
                    gckOS_CreateKernelMapping(os,
                                              node->Virtual.physical,
                                              0,
                                              node->Virtual.bytes,
                                              &node->Virtual.kvaddr));
            }

            logical = node->Virtual.kvaddr;
        }
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));

    *Logical = logical;

    gcmkFOOTER_ARG("*Logical=%p", logical);
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_UnlockCPU(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctUINT32 ProcessID,
    IN gctBOOL FromUser
    )
{
    gceSTATUS status;
    gckOS os = Kernel->os;
    gctBOOL acquired = gcvFALSE;
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    gcmkHEADER_ARG("NodeObject=%p", NodeObject);

    /* Grab the mutex. */
    gcmkONERROR(gckOS_AcquireMutex(os, NodeObject->mutex, gcvINFINITE));
    acquired = gcvTRUE;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        if (FromUser)
        {
            /* Do nothing here. */
        }
        else
        {
            /*
             * Kernel side may lock for CPU access for multiple times. Since
             * we don't have lock counts currently, we don't cancel CPU
             * mapping here, and will cancel at 'free' instead.
             */
            /*
            gcmkONERROR(
                gckOS_DestroyKernelMapping(os,
                                           node->VidMem.parent->physical,
                                           node->VidMem.kvaddr));

            node->VidMem.kvaddr = gcvNULL;
             */
        }
    }
    else
    {
        if (FromUser)
        {
            gcmkONERROR(
                gckOS_UnlockPages(os,
                                  node->Virtual.physical,
                                  node->Virtual.bytes,
                                  node->Virtual.logical));
        }
        else
        {
            /*
             * Kernel side may lock for CPU access for multiple times. Since
             * we don't have lock counts currently, we don't cancel CPU
             * mapping here, and will cancel at 'free' instead.
             */
            /*
            gcmkONERROR(
                gckOS_DestroyKernelMapping(os,
                                           node->Virtual.physical,
                                           node->Virtual.kvaddr));

            node->Virtual.kvaddr = gcvNULL;
             */
        }
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        /* Release the mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(os, NodeObject->mutex));
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_GetPhysical(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctUINT32 Offset,
    OUT gctPHYS_ADDR_T * PhysicalAddress
    )
{
    gceSTATUS status;
    gckOS os = Kernel->os;
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    gcmkHEADER_ARG("NodeObject=%p", NodeObject);

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        if (Offset >= node->VidMem.bytes)
        {
            /* Exceeds node size. */
            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        *PhysicalAddress = node->VidMem.parent->physicalBase
                         + node->VidMem.offset
                         + Offset;
    }
    else
    {
        if (Offset >= node->Virtual.bytes)
        {
            /* Exceeds node size. */
            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        gcmkONERROR(
            gckOS_GetPhysicalFromHandle(os,
                                        node->Virtual.physical,
                                        Offset,
                                        PhysicalAddress));
    }

    gcmkFOOTER_ARG("*PhysicalAddress=0x%llx", *PhysicalAddress);
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_GetGid(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctUINT32 * Gid
    )
{
    gcuVIDMEM_NODE_PTR node = NodeObject->node;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        *Gid = 0;
    }
    else
    {
        *Gid = node->Virtual.gid;
    }

    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_GetSize(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctSIZE_T * Size
    )
{
    gcuVIDMEM_NODE_PTR node;
    node = NodeObject->node;

    *Size = (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
          ? node->VidMem.bytes : node->Virtual.bytes;

    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_GetType(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gceVIDMEM_TYPE * Type,
    OUT gcePOOL * Pool
    )
{
    if (Type)
    {
        *Type = NodeObject->type;
    }

    if (Pool)
    {
        *Pool = NodeObject->pool;
    }

    return gcvSTATUS_OK;
}

#if gcdPROCESS_ADDRESS_SPACE
gceSTATUS
gckVIDMEM_Node_Lock(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE Node,
    OUT gctUINT32 *Address
    )
{
    gceSTATUS           status;
    gckOS               os;
    gcuVIDMEM_NODE_PTR  node = Node->node;
    gcsGPU_MAP_PTR      gpuMap;
    gctPHYS_ADDR        physical = gcvNULL;
    gctUINT32           phys = gcvINVALID_ADDRESS;
    gctUINT32           processID;
    gcsLOCK_INFO_PTR    lockInfo;
    gctUINT32           pageCount;
    gckMMU              mmu;
    gctUINT32           i;
    gctUINT32_PTR       pageTableEntry;
    gctUINT32           offset = 0;
    gctBOOL             acquired = gcvFALSE;

    gcmkHEADER_ARG("Node = %x", Node);

    gcmkVERIFY_OBJECT(Kernel, gcvOBJ_KERNEL);
    gcmkVERIFY_ARGUMENT(Node != gcvNULL);
    gcmkVERIFY_ARGUMENT(Address != gcvNULL);

    os = Kernel->os;
    gcmkVERIFY_OBJECT(os, gcvOBJ_OS);

    gcmkONERROR(gckOS_GetProcessID(&processID));

    gcmkONERROR(gckKERNEL_GetProcessMMU(Kernel, &mmu));

    gcmkONERROR(gckOS_AcquireMutex(os, Node->mapMutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Get map information for current process. */
    gpuMap = _FindGPUMap(Node->mapHead, processID);

    if (gpuMap == gcvNULL)
    {
        gpuMap = _CreateGPUMap(os, &Node->mapHead, &Node->mapTail, processID);

        if (gpuMap == gcvNULL)
        {
            gcmkONERROR(gcvSTATUS_OUT_OF_MEMORY);
        }
    }

    lockInfo = &gpuMap->lockInfo;

    if (lockInfo->lockeds[Kernel->core] ++ == 0)
    {
        /* Get necessary information. */
        if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
        {
            phys = node->VidMem.parent->physicalBase
                 + node->VidMem.offset
                 + node->VidMem.alignment;

            /* GPU page table use 4K page. */
            pageCount = ((phys + node->VidMem.bytes + 4096 - 1) >> 12)
                      - (phys >> 12);

            offset = phys & 0xFFF;
        }
        else
        {
            pageCount = node->Virtual.pageCount;
            physical = node->Virtual.physical;
        }

        /* Allocate pages inside the MMU. */
        gcmkONERROR(gckMMU_AllocatePages(
            mmu,
            pageCount,
            &lockInfo->pageTables[Kernel->core],
            &lockInfo->GPUAddresses[Kernel->core]));

        /* Record MMU from which pages are allocated.  */
        lockInfo->lockMmus[Kernel->core] = mmu;

        pageTableEntry = lockInfo->pageTables[Kernel->core];

        /* Fill page table entries. */
        if (phys != gcvINVALID_ADDRESS)
        {
            gctUINT32 address = lockInfo->GPUAddresses[Kernel->core];
            for (i = 0; i < pageCount; i++)
            {
                gckMMU_GetPageEntry(mmu, address, &pageTableEntry);
                gckMMU_SetPage(mmu, phys & 0xFFFFF000, pageTableEntry);
                phys += 4096;
                address += 4096;
                pageTableEntry += 1;
            }
        }
        else
        {
            gctUINT32 address = lockInfo->GPUAddresses[Kernel->core];
            gcmkASSERT(physical != gcvNULL);
            gcmkONERROR(gckOS_MapPagesEx(os,
                Kernel->core,
                physical,
                pageCount,
                address,
                pageTableEntry));
        }

        gcmkONERROR(gckMMU_Flush(mmu, Node->type));
    }

    *Address = lockInfo->GPUAddresses[Kernel->core] + offset;

    gcmkVERIFY_OK(gckOS_ReleaseMutex(os, Node->mapMutex));
    acquired = gcvFALSE;


    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(os, Node->mapMutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_Unlock(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE Node,
    IN gctUINT32 ProcessID
    )
{
    gceSTATUS           status;
    gcsGPU_MAP_PTR      gpuMap;
    gcsLOCK_INFO_PTR    lockInfo;
    gckMMU              mmu;
    gcuVIDMEM_NODE_PTR  node;
    gctUINT32           pageCount;
    gctBOOL             acquired = gcvFALSE;

    gcmkHEADER_ARG("Kernel=0x%08X, Node = %x, ProcessID=%d",
                   Kernel, Node, ProcessID);

    gcmkVERIFY_OBJECT(Kernel, gcvOBJ_KERNEL);
    gcmkVERIFY_ARGUMENT(Node != gcvNULL);

    gcmkONERROR(gckOS_AcquireMutex(Kernel->os, Node->mapMutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Get map information for current process. */
    gpuMap = _FindGPUMap(Node->mapHead, ProcessID);

    if (gpuMap == gcvNULL)
    {
        /* No mapping for this process. */
        gcmkONERROR(gcvSTATUS_INVALID_DATA);
    }

    lockInfo = &gpuMap->lockInfo;

    if (--lockInfo->lockeds[Kernel->core] == 0)
    {
        node = Node->node;

        /* Get necessary information. */
        if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
        {
            gctUINT32 phys = node->VidMem.parent->physicalBase
                           + node->VidMem.offset
                           + node->VidMem.alignment;

            /* GPU page table use 4K page. */
            pageCount = ((phys + node->VidMem.bytes + 4096 - 1) >> 12)
                      - (phys >> 12);
        }
        else
        {
            pageCount = node->Virtual.pageCount;
        }

        /* Get MMU which allocates pages. */
        mmu = lockInfo->lockMmus[Kernel->core];

        /* Free virtual spaces in page table. */
        gcmkVERIFY_OK(gckMMU_FreePagesEx(
            mmu,
            lockInfo->GPUAddresses[Kernel->core],
            pageCount
            ));

        _DestroyGPUMap(Kernel->os, &Node->mapHead, &Node->mapTail, gpuMap);
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, Node->mapMutex));
    acquired = gcvFALSE;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, Node->mapMutex));
    }

    gcmkFOOTER();
    return status;
}
#endif

#if defined(CONFIG_DMA_SHARED_BUFFER)

/*******************************************************************************
**
**
** Code for dma_buf ops
**
**
*******************************************************************************/

#include <linux/slab.h>
#include <linux/mm_types.h>
#include <linux/dma-buf.h>

static struct sg_table *_dmabuf_map(struct dma_buf_attachment *attachment,
                                    enum dma_data_direction direction)
{
    struct sg_table *sgt = gcvNULL;
    struct dma_buf *dmabuf = attachment->dmabuf;
    gckVIDMEM_NODE nodeObject = dmabuf->priv;
    gceSTATUS status = gcvSTATUS_OK;

    do
    {
        gcuVIDMEM_NODE_PTR node = nodeObject->node;
        gctPHYS_ADDR physical = gcvNULL;
        gctSIZE_T offset = 0;
        gctSIZE_T bytes = 0;

        if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
        {
            physical = node->VidMem.parent->physical;
            offset = node->VidMem.offset;
            bytes = node->VidMem.bytes;
        }
        else
        {
            physical = node->Virtual.physical;
            offset = 0;
            bytes = node->Virtual.bytes;
        }

        gcmkERR_BREAK(gckOS_MemoryGetSGT(nodeObject->kernel->os, physical, offset, bytes, (gctPOINTER*)&sgt));

        if (dma_map_sg(attachment->dev, sgt->sgl, sgt->nents, direction) == 0)
        {
            sg_free_table(sgt);
            kfree(sgt);
            sgt = gcvNULL;
            gcmkERR_BREAK(gcvSTATUS_GENERIC_IO);
        }
    }
    while (gcvFALSE);

    return sgt;
}

static void _dmabuf_unmap(struct dma_buf_attachment *attachment,
                          struct sg_table *sgt,
                          enum dma_data_direction direction)
{
    dma_unmap_sg(attachment->dev, sgt->sgl, sgt->nents, direction);

    sg_free_table(sgt);
    kfree(sgt);
}

static int _dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
    gckVIDMEM_NODE nodeObject = dmabuf->priv;
    gcuVIDMEM_NODE_PTR node = nodeObject->node;
    gctPHYS_ADDR physical = gcvNULL;
    gctSIZE_T skipPages = vma->vm_pgoff;
    gctSIZE_T numPages = PAGE_ALIGN(vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
    gceSTATUS status = gcvSTATUS_OK;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        physical = node->VidMem.parent->physical;
        skipPages += (node->VidMem.offset >> PAGE_SHIFT);
    }
    else
    {
        physical = node->Virtual.physical;
    }

    gcmkONERROR(gckOS_MemoryMmap(nodeObject->kernel->os, physical, skipPages, numPages, vma));

OnError:
    return gcmIS_ERROR(status) ? -EINVAL : 0;
}

static void _dmabuf_release(struct dma_buf *dmabuf)
{
    gckVIDMEM_NODE nodeObject = dmabuf->priv;

    gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(nodeObject->kernel, nodeObject));
}

static void *_dmabuf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
    gckVIDMEM_NODE nodeObject = dmabuf->priv;
    gcuVIDMEM_NODE_PTR node = nodeObject->node;
    gctINT8_PTR kvaddr = gcvNULL;
    gctPHYS_ADDR physical = gcvNULL;
    gctSIZE_T bytes = 0;

    offset = (offset << PAGE_SHIFT);
    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        physical = node->VidMem.parent->physical;
        offset += node->VidMem.offset;
        bytes = node->VidMem.bytes;
    }
    else
    {
        physical = node->Virtual.physical;
        bytes = node->Virtual.bytes;
    }

    if (gcmIS_SUCCESS(gckOS_CreateKernelMapping(
            nodeObject->kernel->os, physical, 0, bytes, (gctPOINTER*)&kvaddr)))
    {
        kvaddr += offset;
    }

    return (gctPOINTER)kvaddr;
}

static void _dmabuf_kunmap(struct dma_buf *dmabuf, unsigned long offset, void *ptr)
{
    gckVIDMEM_NODE nodeObject = dmabuf->priv;
    gcuVIDMEM_NODE_PTR node = nodeObject->node;
    gctINT8_PTR kvaddr = (gctINT8_PTR)ptr - (offset << PAGE_SHIFT);
    gctPHYS_ADDR physical = gcvNULL;

    if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
    {
        physical = node->VidMem.parent->physical;
        kvaddr -= node->VidMem.offset;
    }
    else
    {
        physical = node->Virtual.physical;
    }

    gcmkVERIFY_OK(gckOS_DestroyKernelMapping(
            nodeObject->kernel->os, physical, (gctPOINTER*)&kvaddr));
}

static struct dma_buf_ops _dmabuf_ops =
{
    .map_dma_buf = _dmabuf_map,
    .unmap_dma_buf = _dmabuf_unmap,
    .mmap = _dmabuf_mmap,
    .release = _dmabuf_release,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0)
    .map_atomic = _dmabuf_kmap,
    .unmap_atomic = _dmabuf_kunmap,
    .map = _dmabuf_kmap,
    .unmap = _dmabuf_kunmap,
#  else
    .kmap_atomic = _dmabuf_kmap,
    .kunmap_atomic = _dmabuf_kunmap,
    .kmap = _dmabuf_kmap,
    .kunmap = _dmabuf_kunmap,
#  endif
};
#endif

gceSTATUS
gckVIDMEM_NODE_Export(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    IN gctINT32 Flags,
    OUT gctPOINTER *DmaBuf,
    OUT gctINT32 *FD
    )
{
#if defined(CONFIG_DMA_SHARED_BUFFER)
    gceSTATUS status = gcvSTATUS_OK;
    struct dma_buf *dmabuf = gcvNULL;

    gcmkHEADER_ARG("Kernel=%p NodeObject=0x%x", Kernel, NodeObject);

    dmabuf = NodeObject->dmabuf;
    if (!dmabuf)
    {
        gctSIZE_T bytes = 0;
        gctPHYS_ADDR physical = gcvNULL;
        gcuVIDMEM_NODE_PTR node = NodeObject->node;

        if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
        {
            physical = node->VidMem.parent->physical;
            bytes = node->VidMem.bytes;
        }
        else
        {
            physical = node->Virtual.physical;
            bytes = node->Virtual.bytes;
        }

        /* Donot really get SGT, just check if the allocator support GetSGT. */
        gcmkONERROR(gckOS_MemoryGetSGT(Kernel->os, physical, 0, 0, NULL));

        {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0)
            DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
            exp_info.ops = &_dmabuf_ops;
            exp_info.size = bytes;
            exp_info.flags = Flags;
            exp_info.priv = NodeObject;
            dmabuf = dma_buf_export(&exp_info);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)
            dmabuf = dma_buf_export(NodeObject, &_dmabuf_ops, bytes, Flags, NULL);
#else
            dmabuf = dma_buf_export(NodeObject, &_dmabuf_ops, bytes, Flags);
#endif
        }

        if (IS_ERR(dmabuf))
        {
            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        /* Reference this gckVIDMEM_NODE object. */
        gckVIDMEM_NODE_Reference(Kernel, NodeObject);
        NodeObject->dmabuf = dmabuf;
    }

    if (DmaBuf)
    {
        *DmaBuf = NodeObject->dmabuf;
    }

    if (FD)
    {
        gctINT fd = dma_buf_fd(dmabuf, Flags);

        if (fd < 0)
        {
            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        *FD = fd;
    }

OnError:
    gcmkFOOTER_ARG("*DmaBuf=%p *FD=0x%x", gcmOPT_POINTER(DmaBuf), gcmOPT_VALUE(FD));
    return status;
#else
    gcmkFATAL("The kernel did NOT support CONFIG_DMA_SHARED_BUFFER");
    return gcvSTATUS_NOT_SUPPORTED;
#endif
}

gceSTATUS
gckVIDMEM_NODE_Name(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctUINT32 * Name
    )
{
    gceSTATUS status;
    gctUINT32 name      = 0;
    gctPOINTER database = Kernel->db->nameDatabase;
    gctPOINTER mutex    = Kernel->db->nameDatabaseMutex;
    gctBOOL acquired    = gcvFALSE;
    gctBOOL referenced  = gcvFALSE;
    gcmkHEADER_ARG("Kernel=0x%X NodeObject=%p", Kernel, NodeObject);

    gcmkVERIFY_ARGUMENT(Name != gcvNULL);

    gcmkONERROR(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    gcmkONERROR(gckVIDMEM_NODE_Reference(Kernel, NodeObject));
    referenced = gcvTRUE;

    if (NodeObject->name == 0)
    {
        /* Name this node. */
        gcmkONERROR(gckKERNEL_AllocateIntegerId(database, NodeObject, &name));
        NodeObject->name = name;
    }
    else
    {
        name = NodeObject->name;
    }

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(Kernel, NodeObject));

    *Name = name;

    gcmkFOOTER_ARG("*Name=%d", *Name);
    return gcvSTATUS_OK;

OnError:
    if (referenced)
    {
        gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(Kernel, NodeObject));
    }

    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_Import(
    IN gckKERNEL Kernel,
    IN gctUINT32 Name,
    OUT gckVIDMEM_NODE * NodeObject
    )
{
    gceSTATUS status;
    gckVIDMEM_NODE node = gcvNULL;
    gctPOINTER database = Kernel->db->nameDatabase;
    gctPOINTER mutex    = Kernel->db->nameDatabaseMutex;
    gctBOOL acquired    = gcvFALSE;
    gctBOOL referenced  = gcvFALSE;

    gcmkHEADER_ARG("Kernel=0x%X Name=%d", Kernel, Name);

    gcmkONERROR(gckOS_AcquireMutex(Kernel->os, mutex, gcvINFINITE));
    acquired = gcvTRUE;

    /* Lookup in database to get the node. */
    gcmkONERROR(gckKERNEL_QueryIntegerId(database, Name, (gctPOINTER *)&node));

    /* Reference the node. */
    gcmkONERROR(gckVIDMEM_NODE_Reference(Kernel, node));
    referenced = gcvTRUE;

    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    acquired = gcvFALSE;

    *NodeObject = node;
    gcmkFOOTER_ARG("*NodeObject=%p", node);
    return gcvSTATUS_OK;

OnError:
    if (referenced)
    {
        gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(Kernel, node));
    }

    if (acquired)
    {
        gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, mutex));
    }

    gcmkFOOTER();
    return status;
}


typedef struct _gcsVIDMEM_NODE_FDPRIVATE
{
    gcsFDPRIVATE   base;
    gckKERNEL      kernel;
    gckVIDMEM_NODE node;
}
gcsVIDMEM_NODE_FDPRIVATE;


static gctINT
_ReleaseFdPrivate(
    gcsFDPRIVATE_PTR FdPrivate
    )
{
    /* Cast private info. */
    gcsVIDMEM_NODE_FDPRIVATE * private = (gcsVIDMEM_NODE_FDPRIVATE *) FdPrivate;

    gckVIDMEM_NODE_Dereference(private->kernel, private->node);
    gckOS_Free(private->kernel->os, private);

    return 0;
}


/*******************************************************************************
**
**  gckVIDMEM_NODE_GetFd
**
**  Attach a gckVIDMEM_NODE object to a native fd.
**
**  OUTPUT:
**
**      gctUINT32 * Fd
**          Pointer to a variable receiving a native fd from os.
*/
gceSTATUS
gckVIDMEM_NODE_GetFd(
    IN gckKERNEL Kernel,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctINT * Fd
    )
{
    gceSTATUS status;
    gctBOOL referenced  = gcvFALSE;
    gcsVIDMEM_NODE_FDPRIVATE * fdPrivate = gcvNULL;
    gcmkHEADER_ARG("Kernel=0x%X NodeObject=%d", Kernel, NodeObject);

    /* Reference node object. */
    gcmkVERIFY_OK(gckVIDMEM_NODE_Reference(Kernel, NodeObject));
    referenced = gcvTRUE;

    /* Allocated fd owns a reference. */
    gcmkONERROR(gckOS_Allocate(
        Kernel->os,
        gcmSIZEOF(gcsVIDMEM_NODE_FDPRIVATE),
        (gctPOINTER *)&fdPrivate
        ));

    fdPrivate->base.release = _ReleaseFdPrivate;
    fdPrivate->kernel = Kernel;
    fdPrivate->node = NodeObject;

    /* Allocated fd owns a reference. */
    gcmkONERROR(gckOS_GetFd("vidmem", &fdPrivate->base, Fd));

    gcmkFOOTER_ARG("*Fd=%d", *Fd);
    return gcvSTATUS_OK;

OnError:
    if (referenced)
    {
        gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(Kernel, NodeObject));
    }

    if (fdPrivate)
    {
        gcmkVERIFY_OK(gcmkOS_SAFE_FREE(Kernel->os, fdPrivate));
    }

    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_WrapUserMemory(
    IN gckKERNEL Kernel,
    IN gcsUSER_MEMORY_DESC_PTR Desc,
    IN gceVIDMEM_TYPE Type,
    OUT gckVIDMEM_NODE * NodeObject,
    OUT gctUINT64 * Bytes
    )
{
    gceSTATUS status = gcvSTATUS_OK;
    gckVIDMEM_NODE nodeObject = gcvNULL;
    gctBOOL found = gcvFALSE;

    gcmkHEADER_ARG("Kernel=0x%x", Kernel);

#if defined(CONFIG_DMA_SHARED_BUFFER)
    if (Desc->flag & gcvALLOC_FLAG_DMABUF)
    {
        struct dma_buf *dmabuf;
        int fd = (int)Desc->handle;

        if (fd >= 0)
        {
            /* Import dma buf handle. */
            dmabuf = dma_buf_get(fd);

            Desc->handle = -1;
            Desc->dmabuf = gcmPTR_TO_UINT64(dmabuf);

            dma_buf_put(dmabuf);
        }
        else
        {
            dmabuf = gcmUINT64_TO_PTR(Desc->dmabuf);
        }

        if (dmabuf->ops == &_dmabuf_ops)
        {
            gctBOOL referenced = gcvFALSE;
            nodeObject = dmabuf->priv;

            do
            {
                /* Reference the node. */
                gcmkERR_BREAK(gckVIDMEM_NODE_Reference(Kernel, nodeObject));
                referenced = gcvTRUE;
                found = gcvTRUE;

                *NodeObject = nodeObject;
                *Bytes = (gctUINT64)dmabuf->size;
            }
            while (gcvFALSE);

            if (gcmIS_ERROR(status) && referenced)
            {
                gcmkVERIFY_OK(gckVIDMEM_NODE_Dereference(Kernel, nodeObject));
            }
        }
    }
#endif

    if (!found)
    {
        gckOS os = Kernel->os;
        gcuVIDMEM_NODE_PTR node = gcvNULL;

        do
        {
            gctSIZE_T pageCountCpu = 0;
            gctSIZE_T pageSizeCpu = 0;
            gctPHYS_ADDR_T physicalAddress = 0;

            gcmkVERIFY_OK(gckOS_GetPageSize(os, &pageSizeCpu));

            /* Allocate an gcuVIDMEM_NODE union. */
            gcmkERR_BREAK(gckOS_Allocate(os, gcmSIZEOF(gcuVIDMEM_NODE), (gctPOINTER*)&node));
            gckOS_ZeroMemory(node, gcmSIZEOF(gcuVIDMEM_NODE));

            /* Initialize gcuVIDMEM_NODE union for virtual memory. */
            node->Virtual.kernel = Kernel;

            /* Wrap Memory. */
            gcmkERR_BREAK(
                gckOS_WrapMemory(os,
                                 Desc,
                                 &node->Virtual.bytes,
                                 &node->Virtual.physical,
                                 &node->Virtual.contiguous,
                                 &pageCountCpu));

            /* Get base physical address. */
            gcmkERR_BREAK(
                gckOS_GetPhysicalFromHandle(os,
                                            node->Virtual.physical,
                                            0,
                                            &physicalAddress));

            /* Allocate handle for this video memory. */
            gcmkERR_BREAK(gckVIDMEM_NODE_Construct(
                Kernel,
                node,
                Type,
                gcvPOOL_VIRTUAL,
                &nodeObject
                ));

            node->Virtual.pageCount = (pageCountCpu * pageSizeCpu -
                    (physicalAddress & (pageSizeCpu - 1) & ~(4096 - 1))) >> 12;

            *NodeObject = nodeObject;
            *Bytes = (gctUINT64)node->Virtual.bytes;
        }
        while (gcvFALSE);

        if (gcmIS_ERROR(status) && node)
        {
            /* Free the structure. */
            gcmkVERIFY_OK(gcmkOS_SAFE_FREE(os, node));
        }
    }

    /* Return the status. */
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckVIDMEM_NODE_SetCommitStamp(
    IN gckKERNEL Kernel,
    IN gceENGINE Engine,
    IN gckVIDMEM_NODE NodeObject,
    IN gctUINT64 CommitStamp
    )
{
    NodeObject->sync[Engine].commitStamp = CommitStamp;
    return gcvSTATUS_OK;
}

gceSTATUS
gckVIDMEM_NODE_GetCommitStamp(
    IN gckKERNEL Kernel,
    IN gceENGINE Engine,
    IN gckVIDMEM_NODE NodeObject,
    OUT gctUINT64_PTR CommitStamp
    )
{
    *CommitStamp = NodeObject->sync[Engine].commitStamp;
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckVIDMEM_NODE_Find
**
**  Find gckVIDMEM_NODE object according to GPU address of specified core.
**
**  INPUT:
**
**      gckKERNEL Kernel
**          Kernel object, specifies core.
**
**      gctUINT32 Address
**          GPU address to search.
**
**  OUTPUT:
**
**      gckVIDMEM_NODE * NodeObject
**          Pointer to a variable hold found video memory node.
**
**      gctUINT32 * Offset
**          The offset of specified GPU address in found video memory node.
*/
gceSTATUS
gckVIDMEM_NODE_Find(
    IN gckKERNEL Kernel,
    IN gctUINT32 Address,
    OUT gckVIDMEM_NODE * NodeObject,
    OUT gctUINT32 * Offset
    )
{
    gceSTATUS status = gcvSTATUS_NOT_FOUND;
    gckVIDMEM_NODE nodeObject = gcvNULL;
    gcuVIDMEM_NODE_PTR node = gcvNULL;
    gcsLISTHEAD_PTR pos;

    gcmkVERIFY_OK(
        gckOS_AcquireMutex(Kernel->os,
                           Kernel->db->videoMemListMutex,
                           gcvINFINITE));

    gcmkLIST_FOR_EACH(pos, &Kernel->db->videoMemList)
    {
        nodeObject = (gckVIDMEM_NODE)gcmCONTAINEROF(pos, struct _gcsVIDMEM_NODE, link);
        node = nodeObject->node;

        if (node->VidMem.parent->object.type == gcvOBJ_VIDMEM)
        {
            if (Address >= node->VidMem.address &&
                Address <= node->VidMem.address - 1 + node->VidMem.bytes)
            {
                *NodeObject = nodeObject;

                if (Offset)
                {
                    *Offset = Address - node->VidMem.address;
                }

                status = gcvSTATUS_OK;
                break;
            }
        }
        else
        {
            if (Address >= node->Virtual.addresses[Kernel->core] &&
                (Address <= node->Virtual.addresses[Kernel->core] - 1 + node->Virtual.bytes))
            {
                *NodeObject = nodeObject;

                if (Offset)
                {
                    *Offset = Address - node->Virtual.addresses[Kernel->core];
                }

                status = gcvSTATUS_OK;
                break;
            }
        }
    }

    gcmkVERIFY_OK(
        gckOS_ReleaseMutex(Kernel->os, Kernel->db->videoMemListMutex));

    return status;
}
