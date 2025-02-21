/*******************************************************************************
 * Copyright (c) 2022, 2022 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include <string.h>
#if defined(OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION)
#include <sys/mman.h>
#include <sys/errno.h>
#endif /* OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION */

#include "omrcomp.h"
#include "omrport.h"
#include "omr.h"

#include "EnvironmentBase.hpp"
#include "Forge.hpp"
#include "GCExtensionsBase.hpp"
#include "Math.hpp"
#include "Heap.hpp"
#include "HeapRegionManager.hpp"
#include "ModronAssertions.h"
#include "SparseVirtualMemory.hpp"
#include "SparseAddressOrderedFixedSizeDataPool.hpp"

/****************************************
 * Initialization
 ****************************************
 */

MM_SparseVirtualMemory*
MM_SparseVirtualMemory::newInstance(MM_EnvironmentBase* env, uint32_t memoryCategory, MM_Heap *in_heap)
{
	MM_SparseVirtualMemory* vmem = NULL;
	vmem = (MM_SparseVirtualMemory*)env->getForge()->allocate(sizeof(MM_SparseVirtualMemory), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());

	if (vmem) {
		new (vmem) MM_SparseVirtualMemory(env, in_heap->getPageSize(), in_heap);
		if (!vmem->initialize(env, memoryCategory)) {
			vmem->kill(env);
			vmem = NULL;
		}
	}

	return vmem;
}

bool
MM_SparseVirtualMemory::initialize(MM_EnvironmentBase* env, uint32_t memoryCategory)
{
	uintptr_t in_heap_size = (uintptr_t)_heap->getHeapTop() - (uintptr_t)_heap->getHeapBase();
	uintptr_t maxHeapSize = _heap->getMaximumMemorySize();
	uintptr_t regionSize = _heap->getHeapRegionManager()->getRegionSize();
	uintptr_t regionCount = in_heap_size / regionSize;

	/* Ideally this should be ceilLog2, however this is the best alternative as ceilLog2 currently does not exist */
	uintptr_t ceilLog2 = MM_Math::floorLog2(regionCount) + 1;

	uintptr_t off_heap_size = (uintptr_t)((ceilLog2 * in_heap_size) / 2);
	bool success = MM_VirtualMemory::initialize(env, off_heap_size, NULL, NULL, 0, memoryCategory);

	if (success) {
		void *sparseHeapBase = getHeapBase();
		_sparseDataPool = MM_SparseAddressOrderedFixedSizeDataPool::newInstance(env, sparseHeapBase, off_heap_size);
		if ((NULL == _sparseDataPool) || omrthread_monitor_init_with_name(&_largeObjectVirtualMemoryMutex, 0, "SparseVirtualMemory::_largeObjectVirtualMemoryMutex")) {
			success = false;
		}
	}

	if (success) {
		Trc_MM_SparseVirtualMemory_createSparseVirtualMemory_success((void*)_heap->getHeapTop(), (void*)_heap->getHeapBase(), (void *)in_heap_size, (void *)maxHeapSize, (void *)regionSize, regionCount, (void *)off_heap_size, _sparseDataPool);
	} else {
		Trc_MM_SparseVirtualMemory_createSparseVirtualMemory_failure((void*)_heap->getHeapTop(), (void*)_heap->getHeapBase(), (void *)in_heap_size, (void *)maxHeapSize, (void *)regionSize, regionCount, (void *)off_heap_size, _sparseDataPool);
	}

	return success;
}

void
MM_SparseVirtualMemory::tearDown(MM_EnvironmentBase *env)
{
	if (NULL != _sparseDataPool) {
		_sparseDataPool->kill(env);
		_sparseDataPool = NULL;
	}

	if (NULL != _largeObjectVirtualMemoryMutex) {
		omrthread_monitor_destroy(_largeObjectVirtualMemoryMutex);
		_largeObjectVirtualMemoryMutex = (omrthread_monitor_t) NULL;
	}

	MM_VirtualMemory::tearDown(env);
}

void
MM_SparseVirtualMemory::kill(MM_EnvironmentBase *env)
{
	tearDown(env);
	env->getForge()->free(this);
}

bool
MM_SparseVirtualMemory::updateSparseDataEntryAfterObjectHasMoved(void *dataPtr, void *objPtr)
{
	omrthread_monitor_enter(_largeObjectVirtualMemoryMutex);
	bool ret = _sparseDataPool->updateSparseDataEntryAfterObjectHasMoved(dataPtr, objPtr);
	omrthread_monitor_exit(_largeObjectVirtualMemoryMutex);
	return ret;
}

void *
MM_SparseVirtualMemory::allocateSparseFreeEntryAndMapToHeapObject(void *proxyObjPtr, uintptr_t size)
{
	/* Commiting and decommiting memory sizes must be multiple of pagesize */
	uintptr_t adjustedSize = MM_Math::roundToCeiling(_pageSize, size);

	omrthread_monitor_enter(_largeObjectVirtualMemoryMutex);
	void *sparseHeapAddr = _sparseDataPool->findFreeListEntry(adjustedSize);
	bool success = MM_VirtualMemory::commitMemory(sparseHeapAddr, adjustedSize);

	/* We can likely significantly reduce the list of platforms requiring zeroing after commit - however initially we will take an ultra safe approach */
#if !(defined(LINUX) && defined(J9VM_ARCH_X86))
	OMRZeroMemory(sparseHeapAddr, adjustedSize);
#endif /* !(defined(LINUX) && defined(J9VM_ARCH_X86)) */

	if (NULL != sparseHeapAddr) {
		_sparseDataPool->mapSparseDataPtrToHeapProxyObjectPtr(sparseHeapAddr, proxyObjPtr, adjustedSize);
	} else {
		/* Impossible to get here, there should always be free space at sparse heap */
		Assert_MM_unreachable();
	}

	omrthread_monitor_exit(_largeObjectVirtualMemoryMutex);

	if (success) {
		Trc_MM_SparseVirtualMemory_commitMemory_success(sparseHeapAddr, (void*)adjustedSize, proxyObjPtr);
	} else {
		Trc_MM_SparseVirtualMemory_commitMemory_failure(sparseHeapAddr, (void*)adjustedSize, proxyObjPtr);
		sparseHeapAddr = NULL;
	}

	return sparseHeapAddr;
}

bool
MM_SparseVirtualMemory::freeSparseRegionAndUnmapFromHeapObject(MM_EnvironmentBase* env, void *dataPtr)
{
	uintptr_t dataSize = _sparseDataPool->findObjectDataSizeForSparseDataPtr(dataPtr);
	bool ret = true;

	if ((NULL != dataPtr) && (0 != dataSize)) {
		Assert_MM_true(0 == (dataSize % _pageSize));
		ret = decommitMemory(env, dataPtr, dataSize);
		if (ret) {
			omrthread_monitor_enter(_largeObjectVirtualMemoryMutex);
			ret = (_sparseDataPool->returnFreeListEntry(dataPtr, dataSize) && _sparseDataPool->unmapSparseDataPtrFromHeapProxyObjectPtr(dataPtr));
			omrthread_monitor_exit(_largeObjectVirtualMemoryMutex);
			Trc_MM_SparseVirtualMemory_decommitMemory_success(dataPtr, (void*)dataSize);
		} else {
			Trc_MM_SparseVirtualMemory_decommitMemory_failure(dataPtr, (void*)dataSize);
			/* TODO: Assert Fatal in case of failure? */
			Assert_MM_true(false);
		}
	}

	return ret;
}
bool
MM_SparseVirtualMemory::decommitMemory(MM_EnvironmentBase* env, void* address, uintptr_t size)
{
	bool ret = false;
#if defined(OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION)
	ret = decommitMemoryForDoubleMapping(env, address, size);
#else /* OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION */
	void *highValidAddress = (void *)((uintptr_t)address + size);
	ret = MM_VirtualMemory::decommitMemory(address, size, address, highValidAddress);
#endif /* OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION */

	return ret;
}

#if defined(OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION)
bool
MM_SparseVirtualMemory::decommitMemoryForDoubleMapping(MM_EnvironmentBase* env, void *dataPtr, uintptr_t dataSize)
{
	int rc = 0;
	OMRPORT_ACCESS_FROM_OMRPORT(env->getPortLibrary());
	struct J9PortVmemIdentifier *identifier = _sparseDataPool->findIdentifierForSparseDataPtr(dataPtr);

	if ((0 == identifier->size) || (NULL == identifier->address)) {
		/* Maybe this address was already returned to the pool - Should be unreachable, but should we assert? */
		Assert_MM_unreachable();
	} else {
		Assert_GC_true_with_message4(env, ((identifier->size == dataSize) && (identifier->address == dataPtr)),
			"dataPtr: %p, identifier->address: %p, dataSize: %zu, identifier->size: %zu\n", dataPtr, identifier->address, dataSize, identifier->size);
		Assert_MM_true((getHeapBase() <= dataPtr) && (getHeapTop() > dataPtr));

		rc = omrvmem_release_double_mapped_region(dataPtr, dataSize, identifier);

		if (-1 == rc) {
			Trc_MM_SparseVirtualMemory_releaseDoubleMappedRegion_failure(dataPtr, identifier->address, (void *)dataSize, (void *)identifier->size);
		} else {
			Trc_MM_SparseVirtualMemory_releaseDoubleMappedRegion_success(dataPtr, identifier->address, (void *)dataSize, (void *)identifier->size);
		}
	}
	return -1 != rc;
}

void
MM_SparseVirtualMemory::recordDoubleMapIdentifierForData(void *dataPtr, struct J9PortVmemIdentifier *identifier)
{
	omrthread_monitor_enter(_largeObjectVirtualMemoryMutex);
	_sparseDataPool->recordDoubleMapIdentifierForData(dataPtr, identifier);
	omrthread_monitor_exit(_largeObjectVirtualMemoryMutex);
}
#endif /* OMR_GC_DOUBLE_MAPPING_FOR_SPARSE_HEAP_ALLOCATION */
