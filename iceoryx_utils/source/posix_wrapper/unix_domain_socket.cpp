// Copyright (c) 2020 by Robert Bosch GmbH. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_utils/internal/posix_wrapper/unix_domain_socket.hpp"
#include "iceoryx_utils/cxx/helplets.hpp"
#include "iceoryx_utils/cxx/smart_c.hpp"
#include "iceoryx_utils/platform/socket.hpp"
#include "iceoryx_utils/platform/unistd.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

namespace iox
{
namespace posix
{
constexpr char UnixDomainSocket::PATH_PREFIX[];

UnixDomainSocket::UnixDomainSocket() noexcept
{
    this->m_isInitialized = false;
    this->m_errorValue = IpcChannelError::NOT_INITIALIZED;
}

UnixDomainSocket::UnixDomainSocket(const IpcChannelName_t& name,
                                   const IpcChannelMode mode,
                                   const IpcChannelSide channelSide,
                                   const size_t maxMsgSize,
                                   const uint64_t maxMsgNumber) noexcept
    : UnixDomainSocket(
        NoPathPrefix,
        [&]() -> UdsName_t {
            /// invalid names will be forwarded and handled by the other constructor
            /// separately
            if (!isNameValid(name))
            {
                return name;
            }
            return UdsName_t(PATH_PREFIX).append(iox::cxx::TruncateToCapacity, name);
        }(),
        mode,
        channelSide,
        maxMsgSize,
        maxMsgNumber)
{
}

UnixDomainSocket::UnixDomainSocket(const NoPathPrefix_t,
                                   const UdsName_t& name,
                                   const IpcChannelMode mode,
                                   const IpcChannelSide channelSide,
                                   const size_t maxMsgSize,
                                   const uint64_t maxMsgNumber [[gnu::unused]]) noexcept
    : m_name(name)
    , m_channelSide(channelSide)
{
    if (!isNameValid(name))
    {
        this->m_isInitialized = false;
        this->m_errorValue = IpcChannelError::INVALID_CHANNEL_NAME;
        return;
    }

    if (maxMsgSize > MAX_MESSAGE_SIZE)
    {
        this->m_isInitialized = false;
        this->m_errorValue = IpcChannelError::MAX_MESSAGE_SIZE_EXCEEDED;
    }
    else
    {
        m_maxMessageSize = maxMsgSize;
        auto createResult = createSocket(mode);

        if (!createResult.has_error())
        {
            this->m_isInitialized = true;
            this->m_errorValue = IpcChannelError::UNDEFINED;
            this->m_sockfd = createResult.value();
        }
        else
        {
            this->m_isInitialized = false;
            this->m_errorValue = createResult.get_error();
        }
    }
}

UnixDomainSocket::UnixDomainSocket(UnixDomainSocket&& other) noexcept
{
    *this = std::move(other);
}

UnixDomainSocket::~UnixDomainSocket() noexcept
{
    if (destroy().has_error())
    {
        std::cerr << "unable to cleanup unix domain socket \"" << m_name << "\" in the destructor" << std::endl;
    }
}

UnixDomainSocket& UnixDomainSocket::operator=(UnixDomainSocket&& other) noexcept
{
    if (this != &other)
    {
        if (destroy().has_error())
        {
            std::cerr << "Unable to cleanup unix domain socket \"" << m_name
                      << "\" in the move constructor/move assingment operator" << std::endl;
        }
        m_name = std::move(other.m_name);
        m_channelSide = std::move(other.m_channelSide);
        m_sockfd = std::move(other.m_sockfd);
        m_sockAddr = std::move(other.m_sockAddr);
        m_isInitialized = std::move(other.m_isInitialized);
        m_errorValue = std::move(other.m_errorValue);
        other.m_sockfd = INVALID_FD;
        m_maxMessageSize = std::move(other.m_maxMessageSize);
        moveCreationPatternValues(std::move(other));

        other.m_isInitialized = false;
        other.m_sockfd = INVALID_FD;
    }

    return *this;
}

cxx::expected<bool, IpcChannelError> UnixDomainSocket::unlinkIfExists(const UdsName_t& name) noexcept
{
    return unlinkIfExists(NoPathPrefix, UdsName_t(PATH_PREFIX).append(iox::cxx::TruncateToCapacity, name));
}

cxx::expected<bool, IpcChannelError> UnixDomainSocket::unlinkIfExists(const NoPathPrefix_t,
                                                                      const UdsName_t& name) noexcept
{
    if (!isNameValid(name))
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }

    auto unlinkCall =
        cxx::makeSmartC(unlink, cxx::ReturnMode::PRE_DEFINED_ERROR_CODE, {ERROR_CODE}, {ENOENT}, name.c_str());

    if (!unlinkCall.hasErrors())
    {
        // ENOENT is set if this socket is not known
        return cxx::success<bool>(unlinkCall.getErrNum() != ENOENT);
    }
    else
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INTERNAL_LOGIC_ERROR);
    }
}

cxx::expected<IpcChannelError> UnixDomainSocket::closeFd(int32_t fileDescriptor)
{
    auto closeCall = cxx::makeSmartC(
        closePlatformFileHandle, cxx::ReturnMode::PRE_DEFINED_ERROR_CODE, {ERROR_CODE}, {}, fileDescriptor);

    if (!closeCall.hasErrors())
    {
        if (IpcChannelSide::SERVER == m_channelSide)
        {
            unlink(m_sockAddr.sun_path);
        }

        m_sockfd = INVALID_FD;
        m_isInitialized = false;

        return cxx::success<void>();
    }
    else
    {
        return createErrorFromErrnum(closeCall.getErrNum());
    }
}

cxx::expected<IpcChannelError> UnixDomainSocket::destroy() noexcept
{
    if (m_isInitialized)
    {
        return closeFd(m_sockfd);
    }

    return cxx::success<void>();
}

cxx::expected<IpcChannelError> UnixDomainSocket::send(const std::string& msg) const noexcept
{
    // we also support timedSend. The setsockopt call sets the timeout for all further sendto calls, so we must set
    // it to 0 to turn the timeout off
    return timedSend(msg, units::Duration::fromSeconds(0ULL));
}

cxx::expected<IpcChannelError> UnixDomainSocket::timedSend(const std::string& msg, const units::Duration& timeout) const
    noexcept
{
    if (msg.size() >= m_maxMessageSize) // message sizes with null termination must be smaller than m_maxMessageSize
    {
        return cxx::error<IpcChannelError>(IpcChannelError::MESSAGE_TOO_LONG);
    }

    if (IpcChannelSide::SERVER == m_channelSide)
    {
        std::cerr << "sending on server side not supported for unix domain socket \"" << m_name << "\"" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::INTERNAL_LOGIC_ERROR);
    }

    struct timeval tv = timeout;
#if defined(__APPLE__)
    if (tv.tv_sec != 0 || tv.tv_usec != 0)
    {
        std::cerr
            << "socket: \"" << m_name
            << "\", timedSend with a timeout != 0 is not supported on MacOS. timedSend will behave like send instead."
            << std::endl;
    }
#endif

    auto setsockoptCall = cxx::makeSmartC(setsockopt,
                                          cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                          {static_cast<ssize_t>(ERROR_CODE)},
                                          {EWOULDBLOCK},
                                          m_sockfd,
                                          SOL_SOCKET,
                                          SO_SNDTIMEO,
                                          reinterpret_cast<const char*>(&tv),
                                          static_cast<socklen_t>(sizeof(tv)));

    if (setsockoptCall.hasErrors())
    {
        return createErrorFromErrnum(setsockoptCall.getErrNum());
    }
    else
    {
        auto sendCall = cxx::makeSmartC(sendto,
                                        cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                        {static_cast<ssize_t>(ERROR_CODE)},
                                        {},
                                        m_sockfd,
                                        msg.c_str(),
                                        static_cast<size_t>(msg.size() + 1), // +1 for the \0 at the end
                                        static_cast<int>(0),
                                        nullptr, // socket address not used for a connected SOCK_DGRAM
                                        static_cast<socklen_t>(0));

        if (sendCall.hasErrors())
        {
            return createErrorFromErrnum(sendCall.getErrNum());
        }
        else
        {
            return cxx::success<void>();
        }
    }
}

cxx::expected<std::string, IpcChannelError> UnixDomainSocket::receive() const noexcept
{
    // we also support timedReceive. The setsockopt call sets the timeout for all further recvfrom calls, so we must set
    // it to 0 to turn the timeout off
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    return timedReceive(units::Duration(tv));
}


cxx::expected<std::string, IpcChannelError> UnixDomainSocket::timedReceive(const units::Duration& timeout) const
    noexcept
{
    if (IpcChannelSide::CLIENT == m_channelSide)
    {
        std::cerr << "receiving on client side not supported for unix domain socket \"" << m_name << "\"" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::INTERNAL_LOGIC_ERROR);
    }

    struct timeval tv = timeout;

    auto setsockoptCall = cxx::makeSmartC(setsockopt,
                                          cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                          {static_cast<ssize_t>(ERROR_CODE)},
                                          {EWOULDBLOCK},
                                          m_sockfd,
                                          SOL_SOCKET,
                                          SO_RCVTIMEO,
                                          reinterpret_cast<const char*>(&tv),
                                          static_cast<socklen_t>(sizeof(tv)));

    if (setsockoptCall.hasErrors())
    {
        return createErrorFromErrnum(setsockoptCall.getErrNum());
    }
    else
    {
        char message[MAX_MESSAGE_SIZE + 1];
        auto recvCall = cxx::makeSmartC(recvfrom,
                                        cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                        {static_cast<ssize_t>(ERROR_CODE)},
                                        {EAGAIN},
                                        m_sockfd,
                                        &(message[0]),
                                        MAX_MESSAGE_SIZE,
                                        0,
                                        nullptr,
                                        nullptr);
        message[MAX_MESSAGE_SIZE] = 0;

        if (recvCall.hasErrors())
        {
            return createErrorFromErrnum(recvCall.getErrNum());
        }
        /// we have to handle the timeout separately since it is not actual an
        /// error, it is expected behavior. but we have to still inform the user
        else if (recvCall.getErrNum() == EAGAIN)
        {
            return createErrorFromErrnum(recvCall.getErrNum());
        }
        else
        {
            return cxx::success<std::string>(std::string(message));
        }
    }
}


cxx::expected<int32_t, IpcChannelError> UnixDomainSocket::createSocket(const IpcChannelMode mode) noexcept
{
    // initialize the sockAddr data structure with the provided name
    memset(&m_sockAddr, 0, sizeof(m_sockAddr));
    m_sockAddr.sun_family = AF_LOCAL;
    const uint64_t maxDestinationSize = sizeof(sockaddr_un::sun_path) - 1;
    if (m_name.size() > maxDestinationSize)
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    strncpy(m_sockAddr.sun_path, m_name.c_str(), m_name.size());

    // we currently don't support a IpcChannelMode::NON_BLOCKING, for send and receive timouts can be used, the other
    // calls are blocking
    if (IpcChannelMode::NON_BLOCKING == mode)
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }

    auto socketCall =
        cxx::makeSmartC(socket, cxx::ReturnMode::PRE_DEFINED_ERROR_CODE, {ERROR_CODE}, {}, AF_LOCAL, SOCK_DGRAM, 0);

    if (socketCall.hasErrors())
    {
        return createErrorFromErrnum(socketCall.getErrNum());
    }

    int32_t sockfd = socketCall.getReturnValue();

    if (IpcChannelSide::SERVER == m_channelSide)
    {
        unlink(m_sockAddr.sun_path);

        auto bindCall = cxx::makeSmartC(bind,
                                        cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                        {ERROR_CODE},
                                        {},
                                        sockfd,
                                        reinterpret_cast<struct sockaddr*>(&m_sockAddr),
                                        static_cast<socklen_t>(sizeof(m_sockAddr)));

        if (!bindCall.hasErrors())
        {
            return cxx::success<int32_t>(sockfd);
        }
        else
        {
            auto closeCall = closeFd(sockfd);
            if (closeCall.has_error())
            {
                return cxx::error<IpcChannelError>(closeCall.get_error());
            }
            return createErrorFromErrnum(bindCall.getErrNum());
        }
    }
    else
    {
        // we use a connected socket, this leads to a behavior closer to the message queue (e.g. error if client
        // is created and server not present)
        auto connectCall = cxx::makeSmartC(connect,
                                           cxx::ReturnMode::PRE_DEFINED_ERROR_CODE,
                                           {ERROR_CODE},
                                           {ENOENT},
                                           sockfd,
                                           (struct sockaddr*)&m_sockAddr,
                                           static_cast<socklen_t>(sizeof(m_sockAddr)));

        if (connectCall.hasErrors())
        {
            auto closeCall = closeFd(sockfd);
            if (closeCall.has_error())
            {
                return cxx::error<IpcChannelError>(closeCall.get_error());
            }
            return createErrorFromErrnum(connectCall.getErrNum());
        }
        else if (connectCall.getErrNum() == ENOENT)
        {
            auto closeCall = closeFd(sockfd);
            if (closeCall.has_error())
            {
                return cxx::error<IpcChannelError>(closeCall.get_error());
            }
            return createErrorFromErrnum(connectCall.getErrNum());
        }
        else
        {
            return cxx::success<int32_t>(sockfd);
        }
    }
}

cxx::expected<bool, IpcChannelError> UnixDomainSocket::isOutdated() noexcept
{
    // This is for being API compatible with the message queue, but has no equivalent for socket.
    // We return false to say that the socket is not outdated. If there is a problem,
    // we rely on the other calls and their error returns

    return cxx::success<bool>(false);
}


cxx::error<IpcChannelError> UnixDomainSocket::createErrorFromErrnum(const int32_t errnum) const noexcept
{
    switch (errnum)
    {
    case EACCES:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::ACCESS_DENIED);
    }
    case EAFNOSUPPORT:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }
    case EINVAL:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }
    case EMFILE:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::PROCESS_LIMIT);
    }
    case ENFILE:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::SYSTEM_LIMIT);
    }
    case ENOBUFS:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::OUT_OF_MEMORY);
    }
    case ENOMEM:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::OUT_OF_MEMORY);
    }
    case EPROTONOSUPPORT:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }
    case EADDRINUSE:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::CHANNEL_ALREADY_EXISTS);
    }
    case EBADF:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_FILE_DESCRIPTOR);
    }
    case ENOTSOCK:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_FILE_DESCRIPTOR);
    }
    case EADDRNOTAVAIL:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case EFAULT:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case ELOOP:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case ENAMETOOLONG:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case ENOTDIR:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case ENOENT:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::NO_SUCH_CHANNEL);
    }
    case EROFS:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_CHANNEL_NAME);
    }
    case EIO:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::I_O_ERROR);
    }
    case ENOPROTOOPT:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::INVALID_ARGUMENTS);
    }
    case ECONNREFUSED:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::NO_SUCH_CHANNEL);
    }
    case ECONNRESET:
    {
        return cxx::error<IpcChannelError>(IpcChannelError::CONNECTION_RESET_BY_PEER);
    }
    case EWOULDBLOCK:
    {
        // no error message needed since this is a normal use case
        return cxx::error<IpcChannelError>(IpcChannelError::TIMEOUT);
    }
    default:
    {
        std::cerr << "internal logic error in unix domain socket \"" << m_name << "\" occurred" << std::endl;
        return cxx::error<IpcChannelError>(IpcChannelError::INTERNAL_LOGIC_ERROR);
    }
    }
}

bool UnixDomainSocket::isNameValid(const UdsName_t& name) noexcept
{
    return !(name.empty() || name.size() < SHORTEST_VALID_NAME || name.size() > LONGEST_VALID_NAME);
}


} // namespace posix
} // namespace iox
