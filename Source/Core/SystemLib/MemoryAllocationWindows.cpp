//==============================================================================
// Joe Schutte
//==============================================================================

#if IsWindows_

#include <SystemLib/MemoryAllocation.h>
#include <SystemLib/OSThreading.h>
#include <SystemLib/BasicTypes.h>
#include <SystemLib/Memory.h>
#include <SystemLib/JsAssert.h>

#include <stdio.h>
#include <windows.h>

// -- allocation tracking
#define EnableManualAllocationTracking_ Debug_ && 0
#define AllocationTrackingIncrement_ 4096
#define EnableVerboseLogging_           0
//#define BreakOnAllocation_ 327

namespace Shooty {

    #if EnableManualAllocationTracking_
    struct Allocation {
        void*       address;
        const char* name;
        const char* file;
        int         line;
        uint64      index;
        uint64      size;
    };

    class CAllocationTracking {
    private:
        uint64 index;
        uint   size;
        uint   used;
        Allocation* allocations;
        uint8  spinLock[CacheLineSize_];

        uint64 allocatedMemory;

    public:
        CAllocationTracking();
        ~CAllocationTracking();

        void AddAllocation(void* address, uint64 allocationSize, const char* name, const char* file, int line);
        void RemoveAllocation(void* address);
    };

    //==============================================================================
    CAllocationTracking::CAllocationTracking() {
        size = AllocationTrackingIncrement_;
        used = 0;
        allocations = new Allocation[size];
        index = 0;

        allocatedMemory = 0;

        CreateSpinLock(spinLock);
    }

    //==============================================================================
    CAllocationTracking::~CAllocationTracking() {
        char logstring[2048];

        for (uint scan = 0; scan < used; ++scan) {
            // -- log each leaked allocation
            if (allocations[scan].name != nullptr) {
                sprintf_s(logstring, 2048, "Index (%llu) - Name (%s) - Memory leak (%llu bytes) on line (%d) of file: %s\n", allocations[scan].index, allocations[scan].name, allocations[scan].size, allocations[scan].line, allocations[scan].file);
            }
            else {
                sprintf_s(logstring, 2048, "Index (%llu) - Memory leak (%llu bytes) on line (%d) of file: %s\n", allocations[scan].index, allocations[scan].size, allocations[scan].line, allocations[scan].file);
            }

            OutputDebugStringA(logstring);
        }

        AssertMsg_(used == 0, "Some memory allocations were not released properly");
        delete[] allocations;
    }

    //==============================================================================
    void CAllocationTracking::AddAllocation(void* address, uint64 allocationSize, const char* name, const char* file, int line) {
        EnterSpinLock(spinLock);

        if (used == size) {
            size += AllocationTrackingIncrement_;

            Allocation* newarray = new Allocation[size];
            Memory::Copy(newarray, allocations, used * sizeof(Allocation));

            delete[] allocations;
            allocations = newarray;
        }

        #ifdef BreakOnAllocation_
            if (BreakOnAllocation_ == index) {
                DebugBreak();
            }
        #endif

        allocations[used].address = address;
        allocations[used].name = name;
        allocations[used].file = file;
        allocations[used].line = line;
        allocations[used].index = index++;
        allocations[used].size = allocationSize;
        ++used;

        allocatedMemory += allocationSize;

        #if EnableVerboseLogging_
            char logstring[2048];
            sprintf_s(logstring, 2048, "Allocation (%llu): %s - Total Allocated %llu\n", allocationSize, name, allocatedMemory);
            OutputDebugString(logstring);
        #endif

        LeaveSpinLock(spinLock);
    }

    //==============================================================================
    void CAllocationTracking::RemoveAllocation(void* address) {
        EnterSpinLock(spinLock);

        bool found = false;

        for (uint scan = 0; scan < used; ++scan) {
            if (allocations[scan].address == address) {
                allocatedMemory -= allocations[scan].size;

                #if EnableVerboseLogging_
                char logstring[2048];
                sprintf_s(logstring, 2048, "Free (%llu): %s - Total Allocated %llu\n", allocations[scan].size, allocations[scan].name, allocatedMemory);
                OutputDebugString(logstring);
                #endif

                allocations[scan].address = allocations[used - 1].address;
                allocations[scan].name = allocations[used - 1].name;
                allocations[scan].file = allocations[used - 1].file;
                allocations[scan].line = allocations[used - 1].line;
                allocations[scan].index = allocations[used - 1].index;
                allocations[scan].size = allocations[used - 1].size;

                found = true;
                break;
            }
        }

        AssertMsg_(found, "Unknown memory address released");
        used -= found ? 1 : 0;

        LeaveSpinLock(spinLock);
    }

    CAllocationTracking tracker;
    #endif

    //==============================================================================
    void* ShootyAlignedMalloc(size_t size, size_t alignment, const char* name, const char* file, int line) {

        void* address = _aligned_malloc(size, alignment);

        #if EnableManualAllocationTracking_
            tracker.AddAllocation(address, size, name, file, line);
        #endif

        return address;
    }

    //==============================================================================
    void* ShootyMalloc(size_t size, const char* name, const char* file, int line) {
        void* address = malloc(size);

        #if EnableManualAllocationTracking_
            tracker.AddAllocation(address, size, name, file, line);
        #endif

        return address;
    }

    //==============================================================================
    void ShootyAlignedFree(void* address) {

        #if EnableManualAllocationTracking_
            tracker.RemoveAllocation(address);
        #endif

        _aligned_free(address);
    }

    //==============================================================================
    void ShootyFree(void* address) {
        #if EnableManualAllocationTracking_
            tracker.RemoveAllocation(address);
        #endif

        free(address);
    }

}

#endif