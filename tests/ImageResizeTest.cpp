// Build with ImageResize.cpp using MSVC /std:c++latest /EHsc.
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdio>
#include <vector>
#include "../src/TVTest_Image/ImageResize.h"

bool CheckResize(int SrcWidth, int SrcHeight, int Width, int Height,
	int SrcBpp, int DstBpp, bool TopDown, std::array<BYTE, 3> Color,
	TVTest::ImageLib::ResizeInterpolation Interpolation = TVTest::ImageLib::ResizeInterpolation::Averaging)
{
	BITMAPINFO Info = {};
	Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	Info.bmiHeader.biWidth = SrcWidth;
	Info.bmiHeader.biHeight = TopDown ? -SrcHeight : SrcHeight;
	Info.bmiHeader.biPlanes = 1;
	Info.bmiHeader.biBitCount = static_cast<WORD>(SrcBpp);
	const std::size_t SrcStride = (SrcWidth * SrcBpp + 31) / 32 * 4;
	std::vector<BYTE> Pixels(SrcStride * SrcHeight);
	for (int y = 0; y < SrcHeight; y++) {
		for (int x = 0; x < SrcWidth; x++) {
			BYTE *p = Pixels.data() + y * SrcStride + x * (SrcBpp / 8);
			for (int c = 0; c < 3; c++)
				p[c] = Color[c];
		}
	}
	const HGLOBAL Image = TVTest::ImageLib::ResizeImage(
		&Info, Pixels.data(), nullptr, Width, Height, DstBpp,
		Interpolation);
	if (Image == nullptr)
		return false;
	const auto Header = static_cast<const BITMAPINFOHEADER*>(::GlobalLock(Image));
	if (Header == nullptr) {
		::GlobalFree(Image);
		return false;
	}
	bool Passed = Header->biWidth == Width
		&& Header->biHeight == (TopDown ? -Height : Height)
		&& Header->biBitCount == DstBpp;
	const auto Bits = reinterpret_cast<const BYTE*>(Header + 1);
	const std::size_t Stride = (Width * DstBpp + 31) / 32 * 4;
	for (int y = 0; y < Height && Passed; y++) {
		for (int x = 0; x < Width && Passed; x++) {
			for (int c = 0; c < 3; c++) {
				const BYTE Actual = Bits[y * Stride + x * (DstBpp / 8) + c];
				if (Actual != Color[c]) {
					std::printf("FAIL %dx%d -> %dx%d: channel %d expected %u, got %u\n",
						SrcWidth, SrcHeight, Width, Height, c,
						static_cast<unsigned>(Color[c]), static_cast<unsigned>(Actual));
					Passed = false;
					break;
				}
			}
		}
	}
	::GlobalUnlock(Image);
	::GlobalFree(Image);
	return Passed;
}

int main()
{
	if (!CheckResize(7680, 4320, 320, 180, 24, 24, false, {255, 255, 255})
			|| !CheckResize(7680, 4320, 1, 1, 32, 24, true, {17, 128, 255})
			|| !CheckResize(1920, 1080, 317, 179, 24, 32, true, {37, 143, 239})
			|| !CheckResize(3, 2, 7, 5, 32, 24, false, {0, 127, 255}))
		return 1;
	using TVTest::ImageLib::ResizeInterpolation;
	for (auto Method : {ResizeInterpolation::Lanczos2, ResizeInterpolation::Lanczos3, ResizeInterpolation::Bicubic}) {
		if (!CheckResize(19, 13, 11, 7, 32, 32, true, {37, 143, 239}, Method)
				|| !CheckResize(3, 2, 7, 5, 24, 24, false, {17, 128, 255}, Method))
			return 1;
	}
	std::puts("PASS: 10 resize cases (8K averaging and 24/32-bit Lanczos/Bicubic)");
	return 0;
}
