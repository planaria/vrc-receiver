#pragma once
#include <exception>
#include <string>

namespace vrcrec
{

	class win32_exception : public std::exception
	{
	public:

		explicit win32_exception(HRESULT hresult)
			: std::exception(get_message(hresult).c_str())
			, hresult_(hresult)
		{
		}

	private:

		static std::string get_message(HRESULT hresult)
		{
			LPSTR p;

			std::string msg;
			msg += "win32_exception";

			if (!FormatMessageA(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				hresult,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				reinterpret_cast<LPSTR>(&p),
				0,
				nullptr))
				return msg;

			try
			{
				msg += "\n";
				msg += p;
			}
			catch (...)
			{
				LocalFree(p);
				throw;
			}

			LocalFree(p);

			return msg;
		}

		HRESULT hresult_;

	};

}

#define VRCREC_THROW_WIN32(x) throw vrcrec::win32_exception(x)
#define VRCREC_CHECK_WIN32(x) do { if (!(x)) VRCREC_THROW_WIN32(GetLastError()); } while(false)
#define VRCREC_CHECK_HRESULT(x) do { HRESULT hr___ = (x); if (FAILED(hr___)) VRCREC_THROW_WIN32(hr___); } while(false)
