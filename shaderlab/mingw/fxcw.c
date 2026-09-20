// Minimal fxc replacement: compiles an HLSL file with D3DCompile (Wine builtin under Wine).
// usage: fxcw.exe <in.hlsl> <entry> <profile> <out.cso>
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
	FILE *f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "open fail\n"); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *src = malloc(n + 1); fread(src, 1, n, f); src[n] = 0; fclose(f);
	ID3DBlob *code = NULL, *err = NULL;
	HRESULT hr = D3DCompile(src, n, argv[1], NULL, D3D_COMPILE_STANDARD_FILE_INCLUDE, argv[2], argv[3], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (err) fprintf(stderr, "%.*s\n", (int)err->lpVtbl->GetBufferSize(err), (char*)err->lpVtbl->GetBufferPointer(err));
	if (FAILED(hr)) { fprintf(stderr, "D3DCompile failed 0x%08lx\n", hr); return 1; }
	FILE *o = fopen(argv[4], "wb"); fwrite(code->lpVtbl->GetBufferPointer(code), 1, code->lpVtbl->GetBufferSize(code), o); fclose(o);
	printf("ok %s (%zu bytes)\n", argv[4], (size_t)code->lpVtbl->GetBufferSize(code));
	return 0;
}
