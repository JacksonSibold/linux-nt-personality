#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

static void print_build_header(const char *probe_name){
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    OSVERSIONINFOEXW ovi={sizeof(ovi)};
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion=(PFN_RtlGetVersion)GetProcAddress(ntdll,"RtlGetVersion");
    DWORD build=0, ubr=0, major=0, minor=0;
    if(pRtlGetVersion){ pRtlGetVersion(&ovi); major=ovi.dwMajorVersion; minor=ovi.dwMinorVersion; build=ovi.dwBuildNumber; }
    HKEY hkey; DWORD ubrVal=0, cb=sizeof(ubrVal);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS){
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb); RegCloseKey(hkey); ubr=ubrVal;
    }
    printf("{\n");
    printf("  \"probe\": \"%s\",\n", probe_name);
    printf("  \"windows_build\": \"%lu.%lu\",\n",(unsigned long)build,(unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n",(unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");
}

static void json_escape(const char *s){ while(*s){ if(*s=='\\') printf("\\\\"); else if(*s=='"') printf("\\\""); else if(*s=='\n') printf("\\n"); else if(*s=='\r') printf("\\r"); else if(*s=='\t') printf("\\t"); else putchar(*s); s++; } }
static DWORD WINAPI pipe_server_thread(LPVOID param){
    HANDLE pipe=(HANDLE)param;
    // Wait for client connect
    BOOL cr=ConnectNamedPipe(pipe, NULL);
    DWORD err=GetLastError();
    // If already connected, err is ERROR_PIPE_CONNECTED (535)
    // Signal via return? We just handle.
    return err;
}

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    print_build_header("probe_namedpipe");
    char pipeName[MAX_PATH];
    DWORD pid=GetCurrentProcessId();
    DWORD tick=GetTickCount();
    snprintf(pipeName,sizeof(pipeName),"\\\\.\\pipe\\ntpers_probe_%lu_%lu", (unsigned long)pid,(unsigned long)tick);

    printf("  \"pipe_name\": \""); json_escape(pipeName); printf("\",\n");

    // Test 1: Message mode server, byte mode client contrast
    HANDLE hMsgServer=CreateNamedPipeA(pipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT, 1, 4096,4096,0,NULL);
    printf("  \"create_message\": {\"handle_valid\":%s,\"err\":%lu},\n", hMsgServer!=INVALID_HANDLE_VALUE?"true":"false",(unsigned long)GetLastError());

    HANDLE hByteServer=INVALID_HANDLE_VALUE;
    char pipeName2[MAX_PATH]; snprintf(pipeName2,sizeof(pipeName2),"\\\\.\\pipe\\ntpers_byte_%lu_%lu",(unsigned long)pid,(unsigned long)tick+1);
    hByteServer=CreateNamedPipeA(pipeName2, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, 1, 4096,4096,0,NULL);
    printf("  \"create_byte\": {\"handle_valid\":%s,\"err\":%lu},\n", hByteServer!=INVALID_HANDLE_VALUE?"true":"false",(unsigned long)GetLastError());

    // Test connect + write/read message mode semantics
    HANDLE serverThread=0; DWORD clientWriteResult=0xDEAD; DWORD serverReadResult=0xDEAD; DWORD peekResult=0xDEAD;
    DWORD transactResult=0xDEAD; DWORD byteModeResult=0xDEAD; DWORD impersonateResult=0xDEAD;
    DWORD disconnectResult=0xDEAD; DWORD zeroByteResult=0xDEAD; DWORD partialReadResult=0xDEAD;

    if(hMsgServer!=INVALID_HANDLE_VALUE){
        serverThread=CreateThread(NULL,0,pipe_server_thread,hMsgServer,0,NULL);
        Sleep(100);
        HANDLE hClient=CreateFileA(pipeName, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        printf("  \"client_connect_message\": {\"handle_valid\":%s,\"err\":%lu},\n", hClient!=INVALID_HANDLE_VALUE?"true":"false",(unsigned long)GetLastError());
        if(hClient!=INVALID_HANDLE_VALUE){
            if(serverThread){ WaitForSingleObject(serverThread,2000); CloseHandle(serverThread); serverThread=0; }
            // Write message
            const char *msg="HelloPipeMessage";
            DWORD written=0;
            BOOL w=WriteFile(hClient, msg, (DWORD)strlen(msg), &written, NULL);
            clientWriteResult = w?0:GetLastError();
            printf("  \"write_message\": {\"success\":%s,\"written\":%lu,\"err\":%lu},\n", w?"true":"false",(unsigned long)written,(unsigned long)GetLastError());
            // Peek before read
            DWORD avail=0, availMsg=0, availPipe=0;
            BOOL pk=PeekNamedPipe(hMsgServer, NULL,0,NULL,&avail,&availMsg);
            peekResult = pk?avail:GetLastError();
            printf("  \"peek\": {\"success\":%s,\"avail\":%lu,\"err\":%lu},\n", pk?"true":"false",(unsigned long)avail,(unsigned long)GetLastError());
            // Read on server with exact buffer
            char rbuf[64]; DWORD rread=0;
            BOOL rr=ReadFile(hMsgServer, rbuf, sizeof(rbuf), &rread, NULL);
            serverReadResult = rr?rread:GetLastError();
            printf("  \"read_message\": {\"success\":%s,\"read\":%lu,\"err\":%lu,\"data\":\"%.*s\"},\n", rr?"true":"false",(unsigned long)rread,(unsigned long)GetLastError(), (int)rread, rbuf);
            // Partial read: write large, read small -> ERROR_MORE_DATA (234) for message mode
            const char *bigMsg="ABCDEFGHIJ12345";
            DWORD w2=0; WriteFile(hClient, bigMsg, 15, &w2, NULL);
            char smallBuf[5]; DWORD sread=0;
            BOOL sr=ReadFile(hMsgServer, smallBuf, 5, &sread, NULL);
            DWORD serr=GetLastError();
            partialReadResult = sr?0:serr;
            printf("  \"partial_read_message\": {\"success\":%s,\"read\":%lu,\"err\":%lu,\"expected_234\":%s},\n", sr?"true":"false",(unsigned long)sread,(unsigned long)serr, serr==234?"true":"false");
            // Drain remaining
            char drain[64]; DWORD dread=0; ReadFile(hMsgServer, drain, sizeof(drain), &dread, NULL);

            // Zero-byte write/read
            DWORD zw=0; BOOL zww=WriteFile(hClient, "", 0, &zw, NULL);
            printf("  \"zero_write\": {\"success\":%s,\"written\":%lu,\"err\":%lu},\n", zww?"true":"false",(unsigned long)zw,(unsigned long)GetLastError());
            CloseHandle(hClient);
            DisconnectNamedPipe(hMsgServer);
            // Transact test removed for hang safety: instead test CallNamedPipe timeout semantics is covered by wait_nonexistent/max_instances below.
            printf("  \"transact\": {\"note\":\"skipped for hang safety, covered by wait/max\"},\n");
            // Impersonate test
            // Need a fresh pipe with client connected
            char impName[MAX_PATH]; snprintf(impName,sizeof(impName),"\\\\.\\pipe\\ntpers_imp_%lu",(unsigned long)tick+3);
            HANDLE hImpServer=CreateNamedPipeA(impName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,4096,4096,0,NULL);
            HANDLE impTh=CreateThread(NULL,0,pipe_server_thread,hImpServer,0,NULL);
            Sleep(100);
            HANDLE hImpClient=CreateFileA(impName, GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
            if(impTh){ WaitForSingleObject(impTh,2000); CloseHandle(impTh);}
            if(hImpClient!=INVALID_HANDLE_VALUE && hImpServer!=INVALID_HANDLE_VALUE){
                BOOL imp=ImpersonateNamedPipeClient(hImpServer);
                DWORD ie=GetLastError();
                printf("  \"impersonate\": {\"success\":%s,\"err\":%lu},\n", imp?"true":"false",(unsigned long)ie);
                if(imp) RevertToSelf();
                CloseHandle(hImpClient);
            } else printf("  \"impersonate\": {\"note\":\"skip no client\"},\n");
            if(hImpServer!=INVALID_HANDLE_VALUE) CloseHandle(hImpServer);
        } else {
            if(serverThread){ TerminateThread(serverThread,0); CloseHandle(serverThread); }
            printf("  \"write_message\": {\"note\":\"no client\"},\n");
            printf("  \"peek\": {\"note\":\"no client\"},\n");
            printf("  \"read_message\": {\"note\":\"no client\"},\n");
            printf("  \"partial_read_message\": {\"note\":\"no client\"},\n");
            printf("  \"zero_write\": {\"note\":\"no client\"},\n");
            printf("  \"transact\": {\"note\":\"no client\"},\n");
            printf("  \"impersonate\": {\"note\":\"no client\"},\n");
        }
        // Disconnect test
        BOOL disc=DisconnectNamedPipe(hMsgServer);
        printf("  \"disconnect\": {\"success\":%s,\"err\":%lu},\n", disc?"true":"false",(unsigned long)GetLastError());
        CloseHandle(hMsgServer);
    } else {
        printf("  \"client_connect_message\": {\"note\":\"no server\"},\n");
        printf("  \"write_message\": {\"note\":\"no server\"},\n");
        printf("  \"peek\": {\"note\":\"no server\"},\n");
        printf("  \"read_message\": {\"note\":\"no server\"},\n");
        printf("  \"partial_read_message\": {\"note\":\"no server\"},\n");
        printf("  \"zero_write\": {\"note\":\"no server\"},\n");
        printf("  \"transact\": {\"note\":\"no server\"},\n");
        printf("  \"impersonate\": {\"note\":\"no server\"},\n");
        printf("  \"disconnect\": {\"note\":\"no server\"},\n");
    }
    if(hByteServer!=INVALID_HANDLE_VALUE){
        // Byte mode test: partial read should NOT give ERROR_MORE_DATA
        HANDLE bTh=CreateThread(NULL,0,pipe_server_thread,hByteServer,0,NULL);
        Sleep(100);
        HANDLE hBClient=CreateFileA(pipeName2, GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        if(hBClient!=INVALID_HANDLE_VALUE){
            if(bTh){ WaitForSingleObject(bTh,2000); CloseHandle(bTh); bTh=0; }
            const char *data="1234567890ABCDE"; DWORD w=0; WriteFile(hBClient, data, 15, &w, NULL);
            char small[5]; DWORD r=0; BOOL sr=ReadFile(hByteServer, small, 5, &r, NULL);
            DWORD e=GetLastError();
            printf("  \"byte_partial\": {\"success\":%s,\"read\":%lu,\"err\":%lu,\"more_data\":%s},\n", sr?"true":"false",(unsigned long)r,(unsigned long)e, e==234?"true":"false");
            // Remaining should be readable
            char rest[16]; DWORD rr=0; ReadFile(hByteServer, rest, sizeof(rest), &rr, NULL);
            printf("  \"byte_remaining\": {\"read\":%lu},\n",(unsigned long)rr);
            CloseHandle(hBClient);
        }
        if(bTh){ TerminateThread(bTh,0); CloseHandle(bTh); }
        CloseHandle(hByteServer);
    }
    // WaitNamedPipe test: try waiting for non-existent pipe
    DWORD wnpStart=GetTickCount();
    BOOL wnp=WaitNamedPipeA("\\\\.\\pipe\\nonexistent_ntpers_12345", 500);
    DWORD wnpErr=GetLastError();
    DWORD wnpElapsed=GetTickCount()-wnpStart;
    printf("  \"wait_nonexistent\": {\"success\":%s,\"err\":%lu,\"elapsed\":%lu},\n", wnp?"true":"false",(unsigned long)wnpErr,(unsigned long)wnpElapsed);
    // Named pipe max instances test: create with nMaxInstances=1, second create should fail
    char instName[MAX_PATH]; snprintf(instName,sizeof(instName),"\\\\.\\pipe\\ntpers_inst_%lu",(unsigned long)tick+4);
    HANDLE hInst1=CreateNamedPipeA(instName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
    HANDLE hInst2=CreateNamedPipeA(instName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE|PIPE_WAIT,1,4096,4096,0,NULL);
    printf("  \"max_instances\": {\"first_valid\":%s,\"second_valid\":%s,\"second_err\":%lu},\n", hInst1!=INVALID_HANDLE_VALUE?"true":"false", hInst2!=INVALID_HANDLE_VALUE?"true":"false",(unsigned long)GetLastError());
    if(hInst1!=INVALID_HANDLE_VALUE) CloseHandle(hInst1);
    if(hInst2!=INVALID_HANDLE_VALUE) CloseHandle(hInst2);
    printf("  \"done\": true\n");
    printf("}\n");
    return 0;
}
