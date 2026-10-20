"""One finite native command in a Windows kill-on-close job; no feature parsing."""
import ctypes as C
from ctypes import wintypes as W
import msvcrt
import os
import pathlib
import subprocess
import time

K=C.WinDLL('kernel32',use_last_error=True)
P=C.c_void_p
S=C.c_size_t
Q=C.c_longlong

class BasicLimit(C.Structure):
    _fields_=[('processTime',Q),('jobTime',Q),('flags',W.DWORD),('minWorking',S),('maxWorking',S),('activeLimit',W.DWORD),('affinity',S),('priority',W.DWORD),('scheduling',W.DWORD)]
class IoCounters(C.Structure):
    _fields_=[(name,C.c_ulonglong) for name in ('readOps','writeOps','otherOps','readBytes','writeBytes','otherBytes')]
class ExtendedLimit(C.Structure):
    _fields_=[('basic',BasicLimit),('io',IoCounters),('processMemory',S),('jobMemory',S),('peakProcess',S),('peakJob',S)]
class Accounting(C.Structure):
    _fields_=[('user',Q),('kernel',Q),('periodUser',Q),('periodKernel',Q),('faults',W.DWORD),('total',W.DWORD),('active',W.DWORD),('terminated',W.DWORD)]
class Startup(C.Structure):
    _fields_=[('cb',W.DWORD),('reserved',W.LPWSTR),('desktop',W.LPWSTR),('title',W.LPWSTR),('x',W.DWORD),('y',W.DWORD),('xSize',W.DWORD),('ySize',W.DWORD),('xChars',W.DWORD),('yChars',W.DWORD),('fill',W.DWORD),('flags',W.DWORD),('show',W.WORD),('reservedSize',W.WORD),('reservedBytes',P),('stdin',W.HANDLE),('stdout',W.HANDLE),('stderr',W.HANDLE)]
class StartupEx(C.Structure):
    _fields_=[('startup',Startup),('attributes',P)]
class ProcessInfo(C.Structure):
    _fields_=[('process',W.HANDLE),('thread',W.HANDLE),('pid',W.DWORD),('tid',W.DWORD)]

def api(name,args,result=W.BOOL):
    f=getattr(K,name);f.argtypes=args;f.restype=result;return f
create_job=api('CreateJobObjectW',[P,W.LPCWSTR],W.HANDLE)
set_job=api('SetInformationJobObject',[W.HANDLE,C.c_int,P,W.DWORD])
query_job=api('QueryInformationJobObject',[W.HANDLE,C.c_int,P,W.DWORD,P])
terminate_job=api('TerminateJobObject',[W.HANDLE,W.UINT])
close=api('CloseHandle',[W.HANDLE])
init_attrs=api('InitializeProcThreadAttributeList',[P,W.DWORD,W.DWORD,C.POINTER(S)])
update_attr=api('UpdateProcThreadAttribute',[P,W.DWORD,S,P,S,P,P])
delete_attrs=api('DeleteProcThreadAttributeList',[P],None)
create_process=api('CreateProcessW',[W.LPCWSTR,W.LPWSTR,P,P,W.BOOL,W.DWORD,P,W.LPCWSTR,P,C.POINTER(ProcessInfo)])
wait=api('WaitForSingleObject',[W.HANDLE,W.DWORD],W.DWORD)
get_exit=api('GetExitCodeProcess',[W.HANDLE,C.POINTER(W.DWORD)])
in_job=api('IsProcessInJob',[W.HANDLE,W.HANDLE,C.POINTER(W.BOOL)])
open_process=api('OpenProcess',[W.DWORD,W.BOOL,W.DWORD],W.HANDLE)
query_image=api('QueryFullProcessImageNameW',[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)])

def check(ok):
    if not ok:raise C.WinError(C.get_last_error())

def run(command,output,receipt,timeout=7200):
    """Job assignment is atomic at creation; only stdio handles are inherited.

    No breakaway permission is granted. Unexpected descendants are terminated and
    recorded without changing the native exit after verified scope cleanup. Query active count before return.
    Caller must retain its durable resource lease if treeEmpty cannot be proven.
    """
    if not 0<timeout<=7200:raise ValueError('Finite command bound must be <=7200 seconds')
    application=str(pathlib.Path(command[0]).resolve(strict=True))
    if not pathlib.Path(command[0]).is_absolute():raise ValueError('Explicit absolute executable required')
    job=create_job(None,None);check(job)
    info=ProcessInfo();attributes=None;attrs_ready=False;tracked=[]
    receipt.update(custody='Windows Job Object at CreateProcessW',killOnClose=True,breakawayAllowed=False,treeEmpty=False,timeoutSeconds=timeout)
    def count():
        data=Accounting();check(query_job(job,1,C.byref(data),C.sizeof(data),None))
        receipt['jobTotalProcesses']=data.total
        return data.active
    def snapshot():
        capacity=64
        while True:
            data=C.create_string_buffer(8+capacity*C.sizeof(S))
            ok=query_job(job,3,data,C.sizeof(data),None)
            if ok:
                assigned,returned=(W.DWORD*2).from_buffer(data)
                if assigned<=returned:break
            elif C.get_last_error()!=234:raise C.WinError(C.get_last_error())
            capacity*=2
            if capacity>65536:raise RuntimeError('Owned job PID inventory exceeds finite cleanup capacity')
        rows=[]
        for pid in (S*returned).from_buffer(data,8):
            row={'pid':pid};rows.append(row)
            handle=open_process(0x101000,False,pid)
            if not handle:
                error=C.get_last_error()
                if error!=87:raise C.WinError(error)
                row['exitedBeforeHandleOpen']=True;continue
            try:
                assigned=W.BOOL();check(in_job(handle,job,C.byref(assigned)))
                if not assigned.value:
                    row['exitedBeforeHandleOpen']=True;continue
                size=W.DWORD(32768);image=C.create_unicode_buffer(size.value)
                check(query_image(handle,0,image,C.byref(size)))
                row['image']=image.value
                tracked.append(handle);handle=None
            finally:
                if handle:close(handle)
        receipt['processesAtScopeCleanup']=rows
    def empty():
        deadline=time.monotonic()+30
        while True:
            active=count();receipt['jobActiveProcesses']=active
            if active==0:
                for handle in tracked:
                    remaining=max(0,int((deadline-time.monotonic())*1000))
                    check(wait(handle,remaining)==0)
                receipt['heldProcessHandlesStopped']=True
                receipt['treeEmpty']=True;return
            if time.monotonic()>=deadline:raise RuntimeError('Owned job still has active processes after cleanup')
            time.sleep(.02)
    try:
        limits=ExtendedLimit();limits.basic.flags=0x2000
        check(set_job(job,9,C.byref(limits),C.sizeof(limits)))
        size=S();init_attrs(None,2,0,C.byref(size))
        if size.value==0:raise C.WinError(C.get_last_error())
        attributes=C.create_string_buffer(size.value)
        check(init_attrs(attributes,2,0,C.byref(size)));attrs_ready=True
        jobs=(W.HANDLE*1)(job)
        check(update_attr(attributes,0,0x2000D,jobs,C.sizeof(jobs),None,None))
        with open(os.devnull,'rb') as null:
            handles=[msvcrt.get_osfhandle(null.fileno()),msvcrt.get_osfhandle(output.fileno())]
            inherited=(W.HANDLE*2)(*handles)
            prior=[os.get_handle_inheritable(h) for h in handles]
            try:
                for h in handles:os.set_handle_inheritable(h,True)
                check(update_attr(attributes,0,0x20002,inherited,C.sizeof(inherited),None,None))
                startup=StartupEx();startup.startup.cb=C.sizeof(startup);startup.attributes=C.cast(attributes,P)
                startup.startup.flags=0x100
                startup.startup.stdin=handles[0];startup.startup.stdout=handles[1];startup.startup.stderr=handles[1]
                argv=C.create_unicode_buffer(subprocess.list2cmdline(command))
                check(create_process(application,argv,None,None,True,0x08080000,None,None,C.byref(startup),C.byref(info)))
            finally:
                for h,old in zip(handles,prior):os.set_handle_inheritable(h,old)
        receipt['childPid']=info.pid
        assigned=W.BOOL();check(in_job(info.process,job,C.byref(assigned)))
        if not assigned.value:raise RuntimeError('Native command is outside its custody job')
        receipt['assignedBeforeExecution']=True
        result=wait(info.process,int(timeout*1000))
        if result==258:
            receipt['timedOut']=True;snapshot();check(terminate_job(job,124))
        elif result!=0:raise C.WinError(C.get_last_error())
        else:
            remaining=count();receipt['descendantsAfterParentExit']=remaining
            # A console host can finish just after its client. Give the owned tree
            # a finite natural-exit grace before classifying surviving descendants.
            grace=time.monotonic()+2
            while remaining and time.monotonic()<grace:
                time.sleep(.02);remaining=count()
            receipt['survivingDescendantsAfterGrace']=remaining
            if remaining:
                snapshot();receipt['forcedScopeCleanup']=True
                check(terminate_job(job,74))
        empty()
        check(wait(info.process,30000)==0)
        code=W.DWORD();check(get_exit(info.process,C.byref(code)))
        receipt['processExit']=code.value
        return 124 if receipt.get('timedOut',False) else code.value
    finally:
        try:
            if not receipt['treeEmpty']:
                check(terminate_job(job,74));empty()
        finally:
            for handle in tracked:close(handle)
            if info.thread:close(info.thread)
            if info.process:close(info.process)
            if attrs_ready:delete_attrs(attributes)
            close(job)
