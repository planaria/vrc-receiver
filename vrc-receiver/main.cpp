#include <iostream>
#include <vector>
#include <optional>
#include <thread>
#include <array>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

#include <boost/gil.hpp>
#include <boost/gil/extension/io/png.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/filesystem.hpp>

#include <Windows.h>

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#define _ATL_NO_AUTOMATIC_NAMESPACE
#define _WTL_NO_AUTOMATIC_NAMESPACE

#include <atlbase.h>
#include <atlstr.h>
#include <wtl/atlapp.h>

#define _WTL_NO_CSTRING
#include <wtl/atlmisc.h>

#include <atlwin.h>
#include <wtl/atlframe.h>
#include <wtl/atlcrack.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3d11.h>

#include <shcore.h>
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>

#include "win32_exception.hpp"

namespace vrcrec
{

	class window_finder
	{
	public:

		WTL::CWindowEx find(const ATL::CString& text)
		{
			windows_.clear();

			EnumWindows(&window_finder::enumeate_windows_callback, reinterpret_cast<LPARAM>(this));

			auto is_vrchat = [&](WTL::CWindowEx window)
			{
				if (!window.IsWindow())
				{
					return false;
				}

				if (!window.IsWindowVisible())
				{
					return false;
				}

				ATL::CString str;
				window.GetWindowText(str);

				return str == text;
			};

			auto it = std::find_if(windows_.begin(), windows_.end(), is_vrchat);

			if (it == windows_.end())
			{
				return NULL;
			}

			return *it;
		}

	private:

		static BOOL enumeate_windows_callback(HWND hWnd, LPARAM lParam)
		{
			auto self = reinterpret_cast<window_finder*>(lParam);
			self->windows_.emplace_back(hWnd);
			return TRUE;
		}

		std::vector<WTL::CWindowEx> windows_;

	};

	WTL::CWindowEx find_window(const ATL::CString& text)
	{
		window_finder finder;
		return finder.find(text);
	}

	auto create_direct3d_device()
	{
		UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		winrt::com_ptr<ID3D11Device> d3dDevice;
		VRCREC_CHECK_HRESULT(D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			createDeviceFlags,
			nullptr,
			0,
			D3D11_SDK_VERSION,
			d3dDevice.put(),
			nullptr,
			nullptr));

		auto dxgiDevice = d3dDevice.as<IDXGIDevice>();

		winrt::com_ptr<::IInspectable> device;
		VRCREC_CHECK_HRESULT(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), device.put()));

		return device.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	}

	template <typename T>
	auto get_dxgii_interface(winrt::Windows::Foundation::IInspectable const& object)
	{
		auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

		winrt::com_ptr<T> result;
		VRCREC_CHECK_HRESULT(access->GetInterface(winrt::guid_of<T>(), result.put_void()));

		return result;
	}

	auto create_capture_item(HWND hwnd)
	{
		namespace abi = ABI::Windows::Graphics::Capture;

		auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
		auto interop = factory.as<IGraphicsCaptureItemInterop>();

		winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
		VRCREC_CHECK_HRESULT(interop->CreateForWindow(hwnd, winrt::guid_of<abi::IGraphicsCaptureItem>(), reinterpret_cast<void**>(winrt::put_abi(item))));

		return item;
	}

	void run()
	{
		using namespace std::chrono_literals;

		while (true)
		{
			auto window = find_window(L"VRChat");

			if (!window)
			{
				std::this_thread::sleep_for(1s);
				continue;
			}

			std::cout << "Window found" << std::endl;

			CRect adjust_rect;
			AdjustWindowRectEx(adjust_rect, window.GetStyle(), !!window.GetMenu(), window.GetExStyle());

			auto device = create_direct3d_device();
			auto d3d_device = get_dxgii_interface<ID3D11Device>(device);

			auto capture_item = create_capture_item(window);
			auto pixel_format = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
			auto frame_pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(device, pixel_format, 2, capture_item.Size());

			std::array<uint8_t, 12> signature = { 0x45, 0x23, 0x01, 0xff, 0xab, 0x89, 0x67, 0xff, 0x00, 0xef, 0xcd, 0xff };
			std::array<uint8_t, 12> signature_rev = { 0x00, 0xef, 0xcd, 0xff, 0xab, 0x89, 0x67, 0xff, 0x45, 0x23, 0x01, 0xff };

			std::vector<std::uint8_t> image_data;
			std::optional<std::uint32_t> image_id;
			boost::dynamic_bitset<> image_filled;
			std::optional<std::uint32_t> last_image_size_x;
			std::optional<std::uint32_t> last_image_size_y;
			std::optional<std::uint32_t> last_screen_size_x;
			std::optional<std::uint32_t> last_screen_size_y;

			auto on_frame_arrived = [&](
				const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool& sender,
				const winrt::Windows::Foundation::IInspectable& args)
			{
				auto frame = sender.TryGetNextFrame();
				auto frame_surface = get_dxgii_interface<ID3D11Texture2D>(frame.Surface());

				auto content_size = frame.ContentSize();

				D3D11_TEXTURE2D_DESC desc;
				frame_surface->GetDesc(&desc);
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.BindFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				desc.MiscFlags = 0;
				desc.Usage = D3D11_USAGE_STAGING;

				winrt::com_ptr<ID3D11Texture2D> dst_texture;
				VRCREC_CHECK_HRESULT(d3d_device->CreateTexture2D(&desc, NULL, dst_texture.put()));

				winrt::com_ptr<ID3D11DeviceContext> context;
				d3d_device->GetImmediateContext(context.put());

				context->CopyResource(dst_texture.get(), frame_surface.get());

				D3D11_MAPPED_SUBRESOURCE mapped;
				VRCREC_CHECK_HRESULT(context->Map(dst_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
				auto unmapper = std::shared_ptr<void>(nullptr, [&](auto) { context->Unmap(dst_texture.get(), 0); });

				auto data = static_cast<const std::uint8_t*>(mapped.pData);

				for (UINT y = 0; y < desc.Height; ++y)
				{
					auto row_begin = data + y * mapped.RowPitch;
					auto row_end = row_begin + desc.Width * 4;
					auto minimum_row_size = signature.size() + signature_rev.size() + 6 * sizeof(std::uint32_t) * 2;
					auto it = std::search(row_begin, row_end - minimum_row_size, signature.begin(), signature.end());

					if (it != row_end)
					{
						auto row_offset = it - row_begin;
						auto p = row_begin + row_offset + signature.size();

						auto read_uint32 = [&]()
						{
							auto value =
								static_cast<std::uint32_t>(p[0]) |
								static_cast<std::uint32_t>(p[1]) << 8 |
								static_cast<std::uint32_t>(p[2]) << 16;

							p += 4;
							return value;
						};

						auto screen_size_x = read_uint32();
						auto screen_size_y = read_uint32();
						auto image_size_x = read_uint32();
						auto image_size_y = read_uint32();
						auto data_id = read_uint32();
						auto data_index = read_uint32();

						auto s2_end = row_begin + row_offset + screen_size_x * 4;

						if (s2_end > row_end)
						{
							continue;
						}

						auto s2_begin = s2_end - signature_rev.size();

						if (!std::equal(s2_begin, s2_end, signature_rev.begin(), signature_rev.end()))
						{
							continue;
						}

						auto y2 = y + screen_size_y - 1;

						if (y2 >= desc.Height)
						{
							continue;
						}

						auto s3_begin = data + row_offset + y2 * mapped.RowPitch;
						auto s3_end = s3_begin + signature.size();

						if (!std::equal(s3_begin, s3_end, signature.begin(), signature.end()))
						{
							continue;
						}

						auto s4_end = s3_begin + screen_size_x * 4;
						auto s4_begin = s4_end - signature_rev.size();

						if (!std::equal(s4_begin, s4_end, signature_rev.begin(), signature_rev.end()))
						{
							continue;
						}

						if (image_size_x == 0 || image_size_y == 0 || screen_size_x == 0 || screen_size_y <= 2)
						{
							continue;
						}

						auto image_size = image_size_x * image_size_y;
						auto body_size = screen_size_x * (screen_size_y - 2);
						auto num_chunks = (image_size - 1) / body_size + 1;

						if (data_id != image_id ||
							last_image_size_x != image_size_x ||
							last_image_size_y != image_size_y ||
							screen_size_x != last_screen_size_x ||
							screen_size_y != last_screen_size_y)
						{
							image_id = data_id;
							image_data.resize(image_size_x * image_size_y * 4);
							std::fill(image_data.begin(), image_data.end(), static_cast<std::uint8_t>(0));
							image_filled.resize(num_chunks);
							image_filled.reset();
							last_image_size_x = image_size_x;
							last_image_size_y = image_size_y;
							last_screen_size_x = screen_size_x;
							last_screen_size_y = screen_size_y;
						}

						auto data_index_mod = data_index % num_chunks;

						if (image_filled.test_set(data_index_mod))
						{
							continue;
						}

						std::cout << "Found "
							<< screen_size_x << ","
							<< screen_size_y << ","
							<< image_size_x << ","
							<< image_size_y << ","
							<< data_id << ","
							<< data_index_mod << ","
							<< image_filled.count() << "/" << image_filled.size()
							<< std::endl;

						auto dst_offset = data_index_mod * body_size * 4;

						for (auto i = y2 - 1; i > y; --i)
						{
							if (dst_offset >= image_data.size())
							{
								break;
							}

							auto dst_size = std::min<std::size_t>(screen_size_x * 4, image_data.size() - dst_offset);
							auto dst_begin = image_data.data() + dst_offset;

							auto src_begin = data + row_offset + i * mapped.RowPitch;
							std::copy_n(src_begin, dst_size, dst_begin);

							dst_offset += screen_size_x * 4;
						}

						if (image_filled.all())
						{
							std::cout << "writing..." << std::endl;

							auto pixels_begin = reinterpret_cast<boost::gil::bgra8_pixel_t*>(image_data.data());
							auto pixels_end = pixels_begin + image_size_x * image_size_y;

							for (auto p = pixels_begin; p != pixels_end; ++p)
							{
								(*p)[3] = 0xff;
							}

							auto v = boost::gil::interleaved_view(image_size_x, image_size_y, pixels_begin, image_size_x * 4);

							auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

							std::ostringstream os;
							os << std::put_time(std::localtime(&now), "%Y_%m_%d_%H_%M_%S");
							os << "_" << data_id;
							os << ".png";

							auto target_file = os.str();

							auto temp_file = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
							boost::gil::write_view(temp_file.native(), v, boost::gil::png_tag());

							boost::filesystem::rename(temp_file, target_file);

							std::cout << "completed" << std::endl;
						}

						break;
					}
				}
			};

			auto frame_arrived = frame_pool.FrameArrived(winrt::auto_revoke, on_frame_arrived);
			auto capture_session = frame_pool.CreateCaptureSession(capture_item);
			capture_session.StartCapture();

			while (true)
			{
				MSG msg;
				BOOL ret = GetMessage(&msg, NULL, 0, 0);
				VRCREC_CHECK_WIN32(ret != -1);

				if (!ret)
				{
					break;
				}

				TranslateMessage(&msg);
				DispatchMessage(&msg);

				if (!window.IsWindow())
				{
					PostQuitMessage(0);
				}
			}
		}
	}

}

int main()
{
	try
	{
		std::wcout.imbue(std::locale("", std::locale::ctype));

		vrcrec::run();
	}
	catch (std::exception& e)
	{
		std::cerr << e.what() << std::endl;
	}

	return 0;
}
