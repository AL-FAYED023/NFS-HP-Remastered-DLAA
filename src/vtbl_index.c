/* vtable 인덱스를 헤더 정의에서 직접 계산 (하드코딩 실수 방지) */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define IDX(T, m) ((int)(offsetof(T##Vtbl, m) / sizeof(void*)))

int Vtbl_PSSetShader(void)          { return IDX(ID3D11DeviceContext, PSSetShader); }
int Vtbl_PSSetShaderResources(void) { return IDX(ID3D11DeviceContext, PSSetShaderResources); }
int Vtbl_PSSetConstantBuffers(void) { return IDX(ID3D11DeviceContext, PSSetConstantBuffers); }
int Vtbl_Present(void)              { return IDX(IDXGISwapChain, Present); }
int Vtbl_Present1(void)             { return IDX(IDXGISwapChain1, Present1); }
