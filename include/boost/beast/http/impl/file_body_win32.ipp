//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef BOOST_BEAST_HTTP_IMPL_FILE_BODY_WIN32_IPP
#define BOOST_BEAST_HTTP_IMPL_FILE_BODY_WIN32_IPP

#if BOOST_BEAST_USE_WIN32_FILE

#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/type_traits.hpp>
#include <boost/beast/core/detail/clamp.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/handler_continuation_hook.hpp>
#include <boost/asio/windows/overlapped_ptr.hpp>
#include <boost/make_unique.hpp>
#include <boost/smart_ptr/make_shared_array.hpp>
#include <boost/detail/winapi/basic_types.hpp>
#include <algorithm>
#include <cstring>

namespace boost {
namespace beast {
namespace http {

namespace detail {
template<class, class, bool, class>
class write_some_win32_op;
} // detail

template<>
struct basic_file_body<file_win32>
{
    using file_type = file_win32;

    class reader;
    class writer;

    //--------------------------------------------------------------------------

    class value_type
    {
        friend class reader;
        friend class writer;
        friend struct basic_file_body<file_win32>;

        template<class, class, bool, class>
        friend class detail::write_some_win32_op;
        template<
            class Protocol, bool isRequest, class Fields>
        friend
        std::size_t
        write_some(
            boost::asio::basic_stream_socket<Protocol>& sock,
            serializer<isRequest,
                basic_file_body<file_win32>, Fields>& sr,
            error_code& ec);

        file_win32 file_;
        std::uint64_t size_ = 0;    // cached file size
        std::uint64_t first_;       // starting offset of the range
        std::uint64_t last_;        // ending offset of the range

    public:
        ~value_type() = default;
        value_type() = default;
        value_type(value_type&& other) = default;
        value_type& operator=(value_type&& other) = default;

        bool
        is_open() const
        {
            return file_.is_open();
        }

        std::uint64_t
        size() const
        {
            return size_;
        }

        void
        close();

        void
        open(char const* path, file_mode mode, error_code& ec);

        void
        reset(file_win32&& file, error_code& ec);
    };

    //--------------------------------------------------------------------------

    class reader
    {
        template<class, class, bool, class>
        friend class detail::write_some_win32_op;
        template<
            class Protocol, bool isRequest, class Fields>
        friend
        std::size_t
        write_some(
            boost::asio::basic_stream_socket<Protocol>& sock,
            serializer<isRequest,
                basic_file_body<file_win32>, Fields>& sr,
            error_code& ec);

        value_type& body_;  // The body we are reading from
        std::uint64_t pos_; // The current position in the file
        char buf_[4096];    // Small buffer for reading

    public:
        using const_buffers_type =
            boost::asio::const_buffer;

        template<bool isRequest, class Fields>
        reader(message<isRequest,
                basic_file_body<file_win32>, Fields>& m)
            : body_(m.body())
        {
        }

        void
        init(error_code&)
        {
            BOOST_ASSERT(body_.file_.is_open());
            pos_ = body_.first_;
        }

        boost::optional<std::pair<const_buffers_type, bool>>
        get(error_code& ec)
        {
            std::size_t const n = (std::min)(sizeof(buf_),
                beast::detail::clamp(body_.last_ - pos_));
            if(n == 0)
            {
                ec.assign(0, ec.category());
                return boost::none;
            }
            auto const nread = body_.file_.read(buf_, n, ec);
            if(ec)
                return boost::none;
            BOOST_ASSERT(nread != 0);
            pos_ += nread;
            ec.assign(0, ec.category());
            return {{
                {buf_, nread},          // buffer to return.
                pos_ < body_.last_}};   // `true` if there are more buffers.
        }
    };

    //--------------------------------------------------------------------------

    class writer
    {
        value_type& body_;

    public:
        template<bool isRequest, class Fields>
        explicit
        writer(message<isRequest, basic_file_body, Fields>& m)
            : body_(m.body())
        {
        }

        void
        init(boost::optional<
            std::uint64_t> const& content_length,
                error_code& ec)
        {
            // VFALCO We could reserve space in the file
            boost::ignore_unused(content_length);
            BOOST_ASSERT(body_.file_.is_open());
            ec.assign(0, ec.category());
        }

        template<class ConstBufferSequence>
        std::size_t
        put(ConstBufferSequence const& buffers,
            error_code& ec)
        {
            std::size_t nwritten = 0;
            for(auto buffer : beast::detail::buffers_range(buffers))
            {
                nwritten += body_.file_.write(
                    buffer.data(), buffer.size(), ec);
                if(ec)
                    return nwritten;
            }
            ec.assign(0, ec.category());
            return nwritten;
        }

        void
        finish(error_code& ec)
        {
            ec.assign(0, ec.category());
        }
    };

    //--------------------------------------------------------------------------

    static
    std::uint64_t
    size(value_type const& body)
    {
        return body.size();
    }
};

//------------------------------------------------------------------------------

inline
void
basic_file_body<file_win32>::
value_type::
close()
{
    error_code ignored;
    file_.close(ignored);
}

inline
void
basic_file_body<file_win32>::
value_type::
open(char const* path, file_mode mode, error_code& ec)
{
    file_.open(path, mode, ec);
    if(ec)
        return;
    size_ = file_.size(ec);
    if(ec)
    {
        close();
        return;
    }
    first_ = 0;
    last_ = size_;
}

inline
void
basic_file_body<file_win32>::
value_type::
reset(file_win32&& file, error_code& ec)
{
    if(file_.is_open())
    {
        error_code ignored;
        file_.close(ignored);
    }
    file_ = std::move(file);
    if(file_.is_open())
    {
        size_ = file_.size(ec);
        if(ec)
        {
            close();
            return;
        }
        first_ = 0;
        last_ = size_;
    }
}

//------------------------------------------------------------------------------

namespace detail {

template<class Unsigned>
inline
boost::detail::winapi::DWORD_
lowPart(Unsigned n)
{
    return static_cast<
        boost::detail::winapi::DWORD_>(
            n & 0xffffffff);
}

template<class Unsigned>
inline
boost::detail::winapi::DWORD_
highPart(Unsigned n, std::true_type)
{
    return static_cast<
        boost::detail::winapi::DWORD_>(
            (n>>32)&0xffffffff);
}

template<class Unsigned>
inline
boost::detail::winapi::DWORD_
highPart(Unsigned, std::false_type)
{
    return 0;
}

template<class Unsigned>
inline
boost::detail::winapi::DWORD_
highPart(Unsigned n)
{
    return highPart(n, std::integral_constant<
        bool, (sizeof(Unsigned)>4)>{});
}

class null_lambda
{
public:
    template<class ConstBufferSequence>
    void
    operator()(error_code&,
        ConstBufferSequence const&) const
    {
        BOOST_ASSERT(false);
    }
};

//------------------------------------------------------------------------------

#if BOOST_ASIO_HAS_WINDOWS_OVERLAPPED_PTR

template<
    class Protocol, class Handler,
    bool isRequest, class Fields>
class write_some_win32_op
{
    boost::asio::basic_stream_socket<Protocol>& sock_;
    serializer<isRequest,
        basic_file_body<file_win32>, Fields>& sr_;
    std::size_t bytes_transferred_ = 0;
    Handler h_;
    bool header_ = false;

public:
    write_some_win32_op(write_some_win32_op&&) = default;
    write_some_win32_op(write_some_win32_op const&) = default;

    template<class DeducedHandler>
    write_some_win32_op(
        DeducedHandler&& h,
        boost::asio::basic_stream_socket<Protocol>& s,
        serializer<isRequest,
            basic_file_body<file_win32>,Fields>& sr)
        : sock_(s)
        , sr_(sr)
        , h_(std::forward<DeducedHandler>(h))
    {
    }

    using allocator_type =
        boost::asio::associated_allocator_t<Handler>;

    allocator_type
    get_allocator() const noexcept
    {
        return boost::asio::get_associated_allocator(h_);
    }

    using executor_type = boost::asio::associated_executor_t<
        Handler, decltype(sock_.get_executor())>;

    executor_type get_executor() const noexcept
    {
        return boost::asio::get_associated_executor(
            h_, sock_.get_executor());
    }

    void
    operator()();

    void
    operator()(
        error_code ec,
        std::size_t bytes_transferred = 0);

    friend
    bool asio_handler_is_continuation(write_some_win32_op* op)
    {
        using boost::asio::asio_handler_is_continuation;
        return asio_handler_is_continuation(
            std::addressof(op->h_));
    }
};

template<
    class Protocol, class Handler,
    bool isRequest, class Fields>
void
write_some_win32_op<
    Protocol, Handler, isRequest, Fields>::
operator()()
{
    if(! sr_.is_header_done())
    {
        header_ = true;
        sr_.split(true);
        return detail::async_write_some(
            sock_, sr_, std::move(*this));
    }
    if(sr_.chunked())
    {
        return detail::async_write_some(
            sock_, sr_, std::move(*this));
    }
    auto& r = sr_.reader_impl();
    boost::detail::winapi::DWORD_ const nNumberOfBytesToWrite =
        std::min<boost::detail::winapi::DWORD_>(
            beast::detail::clamp(std::min<std::uint64_t>(
                r.body_.last_ - r.pos_, sr_.limit())),
            2147483646);
    boost::asio::windows::overlapped_ptr overlapped{
        sock_.get_executor().context(), *this};
    auto& ov = *overlapped.get();
    ov.Offset = lowPart(r.pos_);
    ov.OffsetHigh = highPart(r.pos_);
    auto const bSuccess = ::TransmitFile(
        sock_.native_handle(),
        sr_.get().body().file_.native_handle(),
        nNumberOfBytesToWrite,
        0,
        overlapped.get(),
        nullptr,
        0);
    auto const dwError = ::GetLastError();
    if(! bSuccess && dwError !=
        boost::detail::winapi::ERROR_IO_PENDING_)
    {
        // VFALCO This needs review, is 0 the right number?
        // completed immediately (with error?)
        overlapped.complete(error_code{static_cast<int>(
            boost::detail::winapi::GetLastError()),
                system_category()}, 0);
        return;
    }
    overlapped.release();
}

template<
    class Protocol, class Handler,
    bool isRequest, class Fields>
void
write_some_win32_op<
    Protocol, Handler, isRequest, Fields>::
operator()(
    error_code ec, std::size_t bytes_transferred)
{
    bytes_transferred_ += bytes_transferred;
    if(! ec)
    {
        if(header_)
        {
            header_ = false;
            return (*this)();
        }
        auto& r = sr_.reader_impl();
        r.pos_ += bytes_transferred;
        BOOST_ASSERT(r.pos_ <= r.body_.last_);
        if(r.pos_ >= r.body_.last_)
        {
            sr_.next(ec, null_lambda{});
            BOOST_ASSERT(! ec);
            BOOST_ASSERT(sr_.is_done());
            if(! sr_.keep_alive())
                ec = error::end_of_stream;
        }
    }
    h_(ec, bytes_transferred_);
}

#endif

} // detail

//------------------------------------------------------------------------------

template<class Protocol, bool isRequest, class Fields>
std::size_t
write_some(
    boost::asio::basic_stream_socket<Protocol>& sock,
    serializer<isRequest,
        basic_file_body<file_win32>, Fields>& sr,
    error_code& ec)
{
    if(! sr.is_header_done())
    {
        sr.split(true);
        auto const bytes_transferred =
            detail::write_some(sock, sr, ec);
        if(ec)
            return bytes_transferred;
        return bytes_transferred;
    }
    if(sr.chunked())
    {
        auto const bytes_transferred =
            detail::write_some(sock, sr, ec);
        if(ec)
            return bytes_transferred;
        return bytes_transferred;
    }
    auto& r = sr.reader_impl();
    r.body_.file_.seek(r.pos_, ec);
    if(ec)
        return 0;
    boost::detail::winapi::DWORD_ const nNumberOfBytesToWrite =
        std::min<boost::detail::winapi::DWORD_>(
            beast::detail::clamp(std::min<std::uint64_t>(
                r.body_.last_ - r.pos_, sr.limit())),
            2147483646);
    auto const bSuccess = ::TransmitFile(
        sock.native_handle(),
        r.body_.file_.native_handle(),
        nNumberOfBytesToWrite,
        0,
        nullptr,
        nullptr,
        0);
    if(! bSuccess)
    {
        ec.assign(static_cast<int>(
            boost::detail::winapi::GetLastError()),
                system_category());
        return 0;
    }
    r.pos_ += nNumberOfBytesToWrite;
    BOOST_ASSERT(r.pos_ <= r.body_.last_);
    if(r.pos_ < r.body_.last_)
    {
        ec.assign(0, ec.category());
    }
    else
    {
        sr.next(ec, detail::null_lambda{});
        BOOST_ASSERT(! ec);
        BOOST_ASSERT(sr.is_done());
        if(! sr.keep_alive())
            ec = error::end_of_stream;
    }
    return nNumberOfBytesToWrite;
}

#if BOOST_ASIO_HAS_WINDOWS_OVERLAPPED_PTR

template<
    class Protocol,
    bool isRequest, class Fields,
    class WriteHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(
    WriteHandler, void(error_code, std::size_t))
async_write_some(
    boost::asio::basic_stream_socket<Protocol>& sock,
    serializer<isRequest,
        basic_file_body<file_win32>, Fields>& sr,
    WriteHandler&& handler)
{
    boost::asio::async_completion<WriteHandler,
        void(error_code)> init{handler};
    detail::write_some_win32_op<
        Protocol,
        BOOST_ASIO_HANDLER_TYPE(WriteHandler,
            void(error_code, std::size_t)),
        isRequest, Fields>{
            init.completion_handler, sock, sr}();
    return init.result.get();
}

#endif

} // http
} // beast
} // boost

#endif

#endif
