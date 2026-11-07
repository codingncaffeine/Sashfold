// The print dialog (PrintDlg), shown from a thread of its own, and the
// document's pages drawn onto the printer it returns: each page rendered
// by the engine's PDF reader at the printer's resolution, fitted to the
// printable area, and put down with StretchDIBits — as Chrome's reader
// prints, a picture of each page.
#include "platform/Print.h"

#include "core/Bitmap.h"
#include "pdf/Document.h"
#include "pdf/Render.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>

namespace sashfold::platform {

namespace {

std::wstring wide(std::string const& text)
{
    if (text.empty())
        return {};
    int const length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

class DialogPrint final : public PrintRequest {
public:
    DialogPrint(PrintJob job, std::function<void()> wake)
        : m_wake(std::move(wake))
    {
        m_thread = std::thread([this, job = std::move(job)] { run(job); });
    }
    ~DialogPrint() override
    {
        if (m_thread.joinable())
            m_thread.join();
    }
    Outcome outcome() const override { return m_outcome; }

private:
    void finish(Outcome outcome)
    {
        m_outcome = outcome;
        if (m_wake)
            m_wake();
    }

    void run(PrintJob const& job)
    {
        pdf::Document::Status status {};
        std::shared_ptr<pdf::Document const> const document = pdf::Document::open(job.pdf, "", status);
        if (!document || document->page_count() == 0) {
            finish(Outcome::Failed);
            return;
        }
        HWND owner = nullptr;
        if (job.parent_window.rfind("win32:", 0) == 0)
            owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(job.parent_window.substr(6), nullptr, 16)));
        PRINTDLGW dialog {};
        dialog.lStructSize = sizeof dialog;
        dialog.hwndOwner = owner;
        dialog.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE | PD_PAGENUMS;
        dialog.nMinPage = 1;
        dialog.nMaxPage = static_cast<WORD>(std::min<std::size_t>(document->page_count(), 0xFFFF));
        dialog.nFromPage = 1;
        dialog.nToPage = dialog.nMaxPage;
        dialog.nCopies = 1;
        if (!PrintDlgW(&dialog)) {
            finish(CommDlgExtendedError() == 0 ? Outcome::Cancelled : Outcome::Unavailable);
            return;
        }
        HDC const printer = dialog.hDC;
        if (!printer) {
            finish(Outcome::Failed);
            return;
        }
        std::size_t first = 0;
        std::size_t last = document->page_count() - 1;
        if (dialog.Flags & PD_PAGENUMS) {
            first = static_cast<std::size_t>(std::max<WORD>(1, dialog.nFromPage)) - 1;
            last = std::min<std::size_t>(document->page_count(), dialog.nToPage) - 1;
        }
        std::wstring const name = wide(job.title.empty() ? "Sashfold" : job.title);
        DOCINFOW info {};
        info.cbSize = sizeof info;
        info.lpszDocName = name.c_str();
        if (StartDocW(printer, &info) <= 0) {
            DeleteDC(printer);
            finish(Outcome::Failed);
            return;
        }
        int const dpi_x = GetDeviceCaps(printer, LOGPIXELSX);
        int const dpi_y = GetDeviceCaps(printer, LOGPIXELSY);
        int const area_width = GetDeviceCaps(printer, HORZRES);
        int const area_height = GetDeviceCaps(printer, VERTRES);
        bool ok = true;
        for (std::size_t index = first; index <= last && ok; ++index) {
            // The page at the printer's resolution, shrunk to fit the printable
            // area when it is larger (never enlarged), centered.
            pdf::Page const& page = document->page(index);
            pdf::PixelSize const at_dpi = pdf::page_pixel_size(page, dpi_x / 72.0);
            double scale = dpi_x / 72.0;
            if (at_dpi.width > area_width || at_dpi.height > area_height)
                scale *= std::min(static_cast<double>(area_width) / at_dpi.width, static_cast<double>(area_height) / at_dpi.height);
            pdf::RenderOptions options;
            options.scale = scale;
            std::optional<Bitmap> const picture = pdf::render_page(*document, index, options);
            if (!picture) {
                ok = false;
                break;
            }
            if (StartPage(printer) <= 0) {
                ok = false;
                break;
            }
            // Top-down 32-bit rows, as the bitmap holds them.
            BITMAPINFO header {};
            header.bmiHeader.biSize = sizeof header.bmiHeader;
            header.bmiHeader.biWidth = picture->width();
            header.bmiHeader.biHeight = -picture->height();
            header.bmiHeader.biPlanes = 1;
            header.bmiHeader.biBitCount = 32;
            header.bmiHeader.biCompression = BI_RGB;
            std::vector<std::uint8_t> rows(static_cast<std::size_t>(picture->width()) * static_cast<std::size_t>(picture->height()) * 4);
            for (int y = 0; y < picture->height(); ++y) {
                for (int x = 0; x < picture->width(); ++x) {
                    Color const c = picture->pixel(x, y);
                    std::uint8_t* const p = rows.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(picture->width()) + static_cast<std::size_t>(x)) * 4;
                    p[0] = c.b;
                    p[1] = c.g;
                    p[2] = c.r;
                    p[3] = 255;
                }
            }
            // Rendered at the horizontal resolution: the rows stretch to the
            // vertical one where the two differ.
            int const draw_width = picture->width();
            int const draw_height = static_cast<int>(std::lround(picture->height() * static_cast<double>(dpi_y) / dpi_x));
            int const x0 = std::max(0, (area_width - draw_width) / 2);
            int const y0 = std::max(0, (area_height - draw_height) / 2);
            SetStretchBltMode(printer, HALFTONE);
            StretchDIBits(printer, x0, y0, draw_width, draw_height, 0, 0, picture->width(), picture->height(), rows.data(), &header,
                DIB_RGB_COLORS, SRCCOPY);
            if (EndPage(printer) <= 0)
                ok = false;
        }
        if (ok)
            EndDoc(printer);
        else
            AbortDoc(printer);
        DeleteDC(printer);
        if (dialog.hDevMode)
            GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames)
            GlobalFree(dialog.hDevNames);
        finish(ok ? Outcome::Printed : Outcome::Failed);
    }

    std::function<void()> m_wake;
    std::thread m_thread;
    std::atomic<Outcome> m_outcome { Outcome::Pending };
};

}

std::unique_ptr<PrintRequest> print_document(PrintJob job, std::function<void()> wake)
{
    return std::make_unique<DialogPrint>(std::move(job), std::move(wake));
}

}
