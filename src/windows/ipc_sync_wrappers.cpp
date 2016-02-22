/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   windows/ipc_sync_wrappers.cpp
 * \author Andrey Semashev
 * \date   23.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/detail/winapi/access_rights.hpp>
#include <boost/detail/winapi/handles.hpp>
#include <boost/detail/winapi/event.hpp>
#include <boost/detail/winapi/semaphore.hpp>
#include <boost/detail/winapi/wait.hpp>
#include <boost/detail/winapi/dll.hpp>
#include <boost/detail/winapi/time.hpp>
#include <boost/detail/winapi/get_last_error.hpp>
#include <boost/detail/winapi/character_code_conversion.hpp>
#include <windows.h> // for error codes
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/checked_delete.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include "unique_ptr.hpp"
#include "windows/ipc_sync_wrappers.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! Hex character table, defined in dump.cpp
extern const char g_hex_char_table[2][16];

} // namespace aux

namespace ipc {

namespace aux {

//! Converts UTF-8 to UTF-16
std::wstring utf8_to_utf16(const char* str)
{
    std::size_t utf8_len = std::strlen(str);
    if (utf8_len == 0)
        return std::wstring();
    else if (BOOST_UNLIKELY(utf8_len > static_cast< std::size_t >((std::numeric_limits< int >::max)())))
        BOOST_LOG_THROW_DESCR(bad_alloc, "Multibyte string too long");

    int len = boost::detail::winapi::MultiByteToWideChar(boost::detail::winapi::CP_UTF8_, boost::detail::winapi::MB_ERR_INVALID_CHARS_, str, static_cast< int >(utf8_len), NULL, 0);
    if (BOOST_LIKELY(len > 0))
    {
        std::wstring wstr;
        wstr.resize(len);

        len = boost::detail::winapi::MultiByteToWideChar(boost::detail::winapi::CP_UTF8_, boost::detail::winapi::MB_ERR_INVALID_CHARS_, str, static_cast< int >(utf8_len), &wstr[0], len);
        if (BOOST_LIKELY(len > 0))
        {
            return wstr;
        }
    }

    BOOST_LOG_THROW_DESCR(conversion_error, "Failed to convert UTF-8 to UTF-16");
    BOOST_LOG_UNREACHABLE_RETURN(std::wstring());
}

void interprocess_event::create(const wchar_t* name, bool manual_reset, permissions const& perms)
{
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventExW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        name,
        boost::detail::winapi::CREATE_EVENT_MANUAL_RESET_ * manual_reset,
        boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::EVENT_MODIFY_STATE_
    );
#else
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        manual_reset,
        false,
        name
    );
#endif
    if (BOOST_UNLIKELY(h == NULL))
    {
        boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an interprocess event object", (err));
    }

    m_event.init(h);
}

void interprocess_event::create_or_open(const wchar_t* name, bool manual_reset, permissions const& perms)
{
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventExW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        name,
        boost::detail::winapi::CREATE_EVENT_MANUAL_RESET_ * manual_reset,
        boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::EVENT_MODIFY_STATE_
    );
#else
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateEventW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        manual_reset,
        false,
        name
    );
#endif
    if (h == NULL)
    {
        const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        if (BOOST_LIKELY(err == ERROR_ALREADY_EXISTS))
        {
            open(name);
            return;
        }
        else
        {
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an interprocess event object", (err));
        }
    }

    m_event.init(h);
}

void interprocess_event::open(const wchar_t* name)
{
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::OpenEventW(boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::EVENT_MODIFY_STATE_, false, name);
    if (BOOST_UNLIKELY(h == NULL))
    {
        const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to open an interprocess event object", (err));
    }

    m_event.init(h);
}

boost::atomic< interprocess_semaphore::is_semaphore_zero_count_t > interprocess_semaphore::is_semaphore_zero_count(&interprocess_semaphore::is_semaphore_zero_count_init);
interprocess_semaphore::nt_query_semaphore_t interprocess_semaphore::nt_query_semaphore = NULL;

void interprocess_semaphore::create_or_open(const wchar_t* name, permissions const& perms)
{
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateSemaphoreExW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        0, // initial count
        (std::numeric_limits< boost::detail::winapi::LONG_ >::max)(), // max count
        name,
        0u, // flags
        boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::SEMAPHORE_MODIFY_STATE_
    );
#else
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::CreateSemaphoreW
    (
        reinterpret_cast< boost::detail::winapi::SECURITY_ATTRIBUTES_* >(perms.get_native()),
        0, // initial count
        (std::numeric_limits< boost::detail::winapi::LONG_ >::max)(), // max count
        name
    );
#endif
    if (h == NULL)
    {
        boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        if (BOOST_LIKELY(err == ERROR_ALREADY_EXISTS))
        {
            open(name);
            return;
        }
        else
        {
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an interprocess semaphore object", (err));
        }
    }

    m_sem.init(h);
}

void interprocess_semaphore::open(const wchar_t* name)
{
    boost::detail::winapi::HANDLE_ h = boost::detail::winapi::OpenSemaphoreW(boost::detail::winapi::SYNCHRONIZE_ | boost::detail::winapi::SEMAPHORE_MODIFY_STATE_, false, name);
    if (BOOST_UNLIKELY(h == NULL))
    {
        const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to open an interprocess semaphore object", (err));
    }

    m_sem.init(h);
}

bool interprocess_semaphore::is_semaphore_zero_count_init(boost::detail::winapi::HANDLE_ h)
{
    is_semaphore_zero_count_t impl = &interprocess_semaphore::is_semaphore_zero_count_emulated;

    // Check if ntdll.dll provides NtQuerySemaphore, see: http://undocumented.ntinternals.net/index.html?page=UserMode%2FUndocumented%20Functions%2FNT%20Objects%2FSemaphore%2FNtQuerySemaphore.html
    boost::detail::winapi::HMODULE_ ntdll = boost::detail::winapi::GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
    {
        nt_query_semaphore_t ntqs = (nt_query_semaphore_t)boost::detail::winapi::get_proc_address(ntdll, "NtQuerySemaphore");
        if (ntqs)
        {
            nt_query_semaphore = ntqs;
            impl = &interprocess_semaphore::is_semaphore_zero_count_nt_query_semaphore;
        }
    }

    is_semaphore_zero_count.store(impl, boost::memory_order_release);

    return impl(h);
}

bool interprocess_semaphore::is_semaphore_zero_count_nt_query_semaphore(boost::detail::winapi::HANDLE_ h)
{
    semaphore_basic_information info = {};
    NTSTATUS_ err = nt_query_semaphore
    (
        h,
        0u, // SemaphoreBasicInformation
        &info,
        sizeof(info),
        NULL
    );
    if (BOOST_UNLIKELY(err != 0u))
    {
        BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to test an interprocess semaphore object for zero count", (ERROR_INVALID_HANDLE));
    }

    return info.current_count == 0u;
}

bool interprocess_semaphore::is_semaphore_zero_count_emulated(boost::detail::winapi::HANDLE_ h)
{
    const boost::detail::winapi::DWORD_ retval = boost::detail::winapi::WaitForSingleObject(h, 0u);
    if (retval == boost::detail::winapi::wait_timeout)
    {
        return true;
    }
    else if (BOOST_UNLIKELY(retval != boost::detail::winapi::wait_object_0))
    {
        const boost::detail::winapi::DWORD_ err = boost::detail::winapi::GetLastError();
        BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to test an interprocess semaphore object for zero count", (err));
    }

    // Restore the decremented counter
    BOOST_VERIFY(!!boost::detail::winapi::ReleaseSemaphore(h, 1, NULL));

    return false;
}

#if !defined(BOOST_MSVC) || _MSC_VER >= 1800
BOOST_CONSTEXPR_OR_CONST uint32_t interprocess_mutex::lock_flag_bit;
BOOST_CONSTEXPR_OR_CONST uint32_t interprocess_mutex::event_set_flag_bit;
BOOST_CONSTEXPR_OR_CONST uint32_t interprocess_mutex::lock_flag_value;
BOOST_CONSTEXPR_OR_CONST uint32_t interprocess_mutex::event_set_flag_value;
BOOST_CONSTEXPR_OR_CONST uint32_t interprocess_mutex::waiter_count_mask;
#endif

void interprocess_mutex::lock_slow()
{
    uint32_t old_state = m_shared_state->m_lock_state.load(boost::memory_order_relaxed);
    mark_waiting_and_try_lock(old_state);

    if ((old_state & lock_flag_value) != 0u) try
    {
        do
        {
            m_event.wait();
            clear_waiting_and_try_lock(old_state);
        }
        while ((old_state & lock_flag_value) != 0u);
    }
    catch (...)
    {
        m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
        throw;
    }
}

bool interprocess_mutex::lock_slow(boost::detail::winapi::HANDLE_ abort_handle)
{
    uint32_t old_state = m_shared_state->m_lock_state.load(boost::memory_order_relaxed);
    mark_waiting_and_try_lock(old_state);

    if ((old_state & lock_flag_value) != 0u) try
    {
        do
        {
            if (!m_event.wait(abort_handle))
            {
                // Wait was interrupted
                m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
                return false;
            }

            clear_waiting_and_try_lock(old_state);
        }
        while ((old_state & lock_flag_value) != 0u);
    }
    catch (...)
    {
        m_shared_state->m_lock_state.fetch_sub(1u, boost::memory_order_acq_rel);
        throw;
    }

    return true;
}

inline void interprocess_mutex::mark_waiting_and_try_lock(uint32_t& old_state)
{
    uint32_t was_locked, new_state;
    do
    {
        was_locked = (old_state & lock_flag_value);
        if (was_locked)
        {
            // Avoid integer overflows
            if (BOOST_UNLIKELY((old_state & waiter_count_mask) == waiter_count_mask))
                BOOST_LOG_THROW_DESCR(limitation_error, "Too many waiters on an interprocess mutex");

            new_state = old_state + 1u;
        }
        else
        {
            new_state = old_state | lock_flag_value;
        }
    }
    while (!m_shared_state->m_lock_state.compare_exchange_weak(old_state, new_state, boost::memory_order_acq_rel, boost::memory_order_relaxed));
}

inline void interprocess_mutex::clear_waiting_and_try_lock(uint32_t& old_state)
{
    old_state &= ~lock_flag_value;
    old_state |= event_set_flag_value;
    uint32_t new_state;
    do
    {
        new_state = ((old_state & lock_flag_value) ? old_state : ((old_state - 1u) | lock_flag_value)) & ~event_set_flag_value;
    }
    while (!m_shared_state->m_lock_state.compare_exchange_strong(old_state, new_state, boost::memory_order_acq_rel, boost::memory_order_relaxed));
}


bool interprocess_condition_variable::wait(interprocess_mutex::optional_unlock& lock, boost::detail::winapi::HANDLE_ abort_handle)
{
    int32_t waiters = m_shared_state->m_waiters;
    if (waiters < 0)
    {
        // We need to select a new semaphore to block on
        m_current_semaphore = get_unused_semaphore();
        ++m_shared_state->m_generation;
        m_shared_state->m_semaphore_id = m_current_semaphore->m_id;
        waiters = 0;
    }
    else
    {
        // Avoid integer overflow
        if (BOOST_UNLIKELY(waiters >= ((std::numeric_limits< int32_t >::max)() - 1)))
            BOOST_LOG_THROW_DESCR(limitation_error, "Too many waiters on an interprocess condition variable");
    }

    m_shared_state->m_waiters = waiters + 1;
    const uint32_t generation = m_shared_state->m_generation;
    semaphore_info* const sem_info = m_current_semaphore;

    interprocess_mutex* const mutex = lock.disengage();
    mutex->unlock();

    const bool result = sem_info->m_semaphore.wait(abort_handle);

    // Have to unconditionally lock the mutex here
    mutex->lock();
    lock.engage(*mutex);

    if (!result && generation == m_shared_state->m_generation && m_shared_state->m_waiters > 0)
        --m_shared_state->m_waiters;

    return result;
}

//! Finds or opens a semaphore with the specified id
interprocess_condition_variable::semaphore_info* interprocess_condition_variable::get_semaphore(uint32_t id)
{
    semaphore_info_set::insert_commit_data insert_state;
    std::pair< semaphore_info_set::iterator, bool > res = m_semaphore_info_set.insert_check(id, semaphore_info::order_by_id(), insert_state);
    if (res.second)
    {
        // We need to open the semaphore. It is possible that the semaphore does not exist because all processes that had it opened terminated.
        // Because of this we also attempt to create it.
        boost::log::aux::unique_ptr< semaphore_info > p(new semaphore_info(id));
        generate_semaphore_name(id);
        p->m_semaphore.create_or_open(m_semaphore_name.c_str(), m_perms);

        res.first = m_semaphore_info_set.insert_commit(*p, insert_state);
        m_semaphore_info_list.push_back(*p);
        p.release();
    }
    else
    {
        // Move the semaphore to the end of the list so that the next time we are less likely to use it
        semaphore_info& info = *res.first;
        m_semaphore_info_list.erase(m_semaphore_info_list.iterator_to(info));
        m_semaphore_info_list.push_back(info);
    }

    return &*res.first;
}

//! Finds or creates a semaphore with zero counter
interprocess_condition_variable::semaphore_info* interprocess_condition_variable::get_unused_semaphore()
{
    // Be optimistic, check the current semaphore first
    if (m_current_semaphore && m_current_semaphore->m_semaphore.is_zero_count())
    {
        mark_unused(*m_current_semaphore);
        return m_current_semaphore;
    }

    const tick_count_clock::time_point now = tick_count_clock::now();

    semaphore_info_list::iterator it = m_semaphore_info_list.begin(), end = m_semaphore_info_list.end();
    while (it != end)
    {
        if (is_overflow_less(m_next_semaphore_id, it->m_id) || m_next_semaphore_id == it->m_id)
            m_next_semaphore_id = it->m_id + 1u;

        if (it->m_semaphore.is_zero_count())
        {
            semaphore_info& info = *it;
            mark_unused(info);
            return &info;
        }
        else if (it->check_non_zero_timeout(now))
        {
            // The semaphore is non-zero for too long. A blocked process must have crashed. Close it.
            m_semaphore_info_set.erase(m_semaphore_info_set.iterator_to(*it));
            m_semaphore_info_list.erase_and_dispose(it++, boost::checked_deleter< semaphore_info >());
        }
        else
        {
            ++it;
        }
    }

    // No semaphore found, create a new one
    for (uint32_t semaphore_id = m_next_semaphore_id, semaphore_id_end = semaphore_id - 1u; semaphore_id != semaphore_id_end; ++semaphore_id)
    {
        interprocess_semaphore sem;
        try
        {
            generate_semaphore_name(semaphore_id);
            sem.create_or_open(m_semaphore_name.c_str(), m_perms);
            if (!sem.is_zero_count())
                continue;
        }
        catch (...)
        {
            // Ignore errors, try the next one
            continue;
        }

        semaphore_info* p = NULL;
        semaphore_info_set::insert_commit_data insert_state;
        std::pair< semaphore_info_set::iterator, bool > res = m_semaphore_info_set.insert_check(semaphore_id, semaphore_info::order_by_id(), insert_state);
        if (res.second)
        {
            p = new semaphore_info(semaphore_id);
            p->m_semaphore.swap(sem);

            res.first = m_semaphore_info_set.insert_commit(*p, insert_state);
            m_semaphore_info_list.push_back(*p);
        }
        else
        {
            // Some of our currently open semaphores must have been released by another thread
            p = &*res.first;
            mark_unused(*p);
        }

        m_next_semaphore_id = semaphore_id + 1u;

        return p;
    }

    BOOST_LOG_THROW_DESCR(limitation_error, "Too many semaphores are actively used for an interprocess condition variable");
    BOOST_LOG_UNREACHABLE_RETURN(NULL);
}

//! Marks the semaphore info as unused and moves to the end of list
inline void interprocess_condition_variable::mark_unused(semaphore_info& info) BOOST_NOEXCEPT
{
    // Restart the timeout for non-zero state next time we search for an unused semaphore
    info.m_checked_for_zero = false;
    // Move to the end of the list so that we consider this semaphore last
    m_semaphore_info_list.erase(m_semaphore_info_list.iterator_to(info));
    m_semaphore_info_list.push_back(info);
}

//! Generates semaphore name according to id
inline void interprocess_condition_variable::generate_semaphore_name(uint32_t id) BOOST_NOEXCEPT
{
    // Note: avoid anything that involves locale to make semaphore names as stable as possible
    BOOST_ASSERT(m_semaphore_name.size() >= 8u);

    wchar_t* p = &m_semaphore_name[m_semaphore_name.size() - 8u];
    *p++ = boost::log::aux::g_hex_char_table[0][id >> 28];
    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 24) & 0x0000000Fu];

    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 20) & 0x0000000Fu];
    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 16) & 0x0000000Fu];

    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 12) & 0x0000000Fu];
    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 8) & 0x0000000Fu];

    *p++ = boost::log::aux::g_hex_char_table[0][(id >> 4) & 0x0000000Fu];
    *p = boost::log::aux::g_hex_char_table[0][id & 0x0000000Fu];
}

} // namespace aux

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
