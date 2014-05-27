/*************************************************************************
 *
 * TIGHTDB CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2014] TightDB Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of TightDB Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to TightDB Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from TightDB Incorporated.
 *
 **************************************************************************/

#include <exception>

#include <tightdb/util/unique_ptr.hpp>
#include <tightdb/util/thread.hpp>
#include <tightdb/group_shared.hpp>
#include <map>
#include <tightdb/commit_log.hpp>

#include <tightdb/replication.hpp>

namespace tightdb {

typedef uint_fast64_t version_type;

class WriteLogRegistry
{
    struct CommitEntry { std::size_t sz; char* data; };
    struct Interest {
        version_type last_seen_version; 
        int next_free_entry; // free-list ptr, terminated by -1, in-use indicated by -2
    };
public:
    WriteLogRegistry();
    ~WriteLogRegistry();
  
    void add_commit(version_type version, char* data, std::size_t sz);
    void get_commit_entries(version_type from, version_type to, BinaryData* commits) TIGHTDB_NOEXCEPT;
    int register_interest(); 
    void unregister_interest(int interest_registration_id);
    void release_commit_entries(int interest_registration_id, 
                                version_type to) TIGHTDB_NOEXCEPT;

private:
    // cleanup and release unreferenced buffers. Buffers might be big, so
    // we release them asap. Only to be called under lock.
    void cleanup();

    // Get the index into the arrays for the selected version.
    size_t to_index(version_type version) { return version - m_array_start; }
    version_type to_version(size_t idx)   { return idx + m_array_start; }
    bool is_a_known_commit(version_type version);
    bool is_interesting(version_type version);
    version_type newest_version() { 
        return to_version(m_commits.size() + to_index(m_oldest_version) - 1 ); 
    }

    // the Moooootex :-)
    util::Mutex m_mutex;

    // array holding all commits. Array start with version 'm_array_start',
    // valid entries stretches from 'm_oldest_version' to the end of the array.
    std::vector<CommitEntry> m_commits;
    version_type m_array_start;
    version_type m_oldest_version;

    // array of all expressed interests - one record for each.
    std::vector<Interest> m_interests;
    int m_interest_free_list; // -1 for empty
    int m_laziest_reader; // -1 for empty, otherwise index of the
    // interest record with lowest 'last_seen_version'.
};


bool WriteLogRegistry::is_a_known_commit(version_type version)
{
    return (version >= m_oldest_version && version <= newest_version());
}

bool WriteLogRegistry::is_interesting(version_type version)
{
    if (m_laziest_reader == -1)
        return false;
    if (version > m_interests[m_laziest_reader].last_seen_version)
        return true;
    return false;
}

WriteLogRegistry::WriteLogRegistry()
{
     m_oldest_version = 0;
     // a version of 0 is never added, so having m_oldest_version==0 indicates that no version
     // has been added yet.
     m_array_start = 0;
     m_interest_free_list = -1;
     m_laziest_reader = -1;
}


WriteLogRegistry::~WriteLogRegistry()
{
    for (size_t i = 0; i < m_commits.size(); i++) {
        if (m_commits[i].data) {
            delete[] m_commits[i].data;
            m_commits[i].data = 0;
        }
    }
}

void WriteLogRegistry::add_commit(version_type version, char* data, std::size_t sz)
{
    util::LockGuard lock(m_mutex);

    // if no one is interested, discard data immediately
    if (!is_interesting(version)) {
        delete[] data;
        return;
    }

    // we assume that commits are entered in version order.
    TIGHTDB_ASSERT(m_oldest_version == 0 || version == 1 + newest_version());
    if (m_oldest_version == 0) {
        m_array_start = version;
        m_oldest_version = version;
    }
    CommitEntry ce = { sz, data };
    m_commits.push_back(ce);
}
    

int WriteLogRegistry::register_interest()
{
    util::LockGuard lock(m_mutex);
    unsigned int retval;
    if (m_interest_free_list != -1) {
        retval = m_interest_free_list;
        m_interest_free_list = m_interests[m_interest_free_list].next_free_entry;
        m_interests[retval].last_seen_version = 0;
    } 
    else {
        Interest i;
        i.last_seen_version = 0;
        m_interests.push_back(i);
        retval = m_interests.size() -1;
    }
    m_interests[retval].next_free_entry = -2; // mark as in-use
    m_laziest_reader = retval;
    return retval;
}
    

void WriteLogRegistry::unregister_interest(int interest_registration_id)
{
    util::LockGuard lock(m_mutex);
    m_interests[interest_registration_id].next_free_entry = m_interest_free_list;
    m_interest_free_list = interest_registration_id;
    if (interest_registration_id == m_laziest_reader)
        cleanup();
}


void WriteLogRegistry::get_commit_entries(version_type from, version_type to, BinaryData* commits)
TIGHTDB_NOEXCEPT
{
    util::LockGuard lock(m_mutex);
    size_t dest_idx = 0;

    for (version_type version = from+1; version <= to; version++) {
        TIGHTDB_ASSERT(is_interesting(version));
        TIGHTDB_ASSERT(is_a_known_commit(version));
        size_t idx = to_index(version);
        WriteLogRegistry::CommitEntry* entry = & m_commits[ idx ];
        commits[dest_idx].set(entry->data, entry->sz);
        dest_idx++;
    }
}
    

void WriteLogRegistry::release_commit_entries(int interest_registration_id, 
                                              version_type to) TIGHTDB_NOEXCEPT
{
    util::LockGuard lock(m_mutex);
    m_interests[interest_registration_id].last_seen_version = to;
    if (interest_registration_id == m_laziest_reader)
        cleanup();
}


void WriteLogRegistry::cleanup()
{
    // locate laziest reader as it may have changed - take care to handle lack of readers
    version_type earliest = newest_version() + 1; // as this version is not present, noone can have seen it
    m_laziest_reader = -1;
    for (size_t i = 0; i < m_interests.size(); i++) {
        if (m_interests[i].next_free_entry == -2 && m_interests[i].last_seen_version < earliest) {
            m_laziest_reader = i;
            earliest = m_interests[i].last_seen_version;
        }
    }
    // bail out early if no versions are registered
    if (m_oldest_version == 0) return;

    // cleanup retained versions up to and including the earliest/oldest version seen by all
    size_t last_to_clean;
    if (m_laziest_reader == -1)
        last_to_clean = newest_version();
    else
        last_to_clean = earliest;
    for (version_type version = m_oldest_version; version <= last_to_clean; version++) {
        size_t idx = to_index(version);
        if (m_commits[idx].data)
            delete[] m_commits[idx].data;
        m_commits[idx].data = 0;
        m_commits[idx].sz = 0;
    }

    if (m_laziest_reader == -1 || last_to_clean == newest_version()) {
        // no interest, or no buffered commits:
        m_oldest_version = 0;
        m_array_start = 0;
        m_commits.resize(0);
    } else {
        m_oldest_version = last_to_clean + 1;

        if (to_index(m_oldest_version) > (m_commits.size() >> 1)) {
            // more than half of the commit array is free, so we'll
            // shift contents down and resize the array.
            size_t copy_start_idx = to_index(m_oldest_version);
            std::copy(& m_commits[ copy_start_idx ],
                      & m_commits[ m_commits.size() + 1 ],
                      & m_commits[ 0 ]);
            m_commits.resize(m_commits.size() - copy_start_idx + 1);
            m_array_start = m_oldest_version;
        }
    }
}







class RegistryRegistry {
public:
    WriteLogRegistry* get(std::string filepath);
    void add(std::string filepath, WriteLogRegistry* registry);
    void remove(std::string filepath);
    ~RegistryRegistry();
private:
    util::Mutex m_mutex;
    std::map<std::string, WriteLogRegistry*> m_registries;
};


WriteLogRegistry* RegistryRegistry::get(std::string filepath)
{
    util::LockGuard lock(m_mutex);
    std::map<std::string, WriteLogRegistry*>::iterator iter;
    iter = m_registries.find(filepath);
    if (iter != m_registries.end())
        return iter->second;
    WriteLogRegistry* result = new WriteLogRegistry;
    m_registries[filepath] = result;
    return result;
}

RegistryRegistry::~RegistryRegistry()
{
    std::map<std::string, WriteLogRegistry*>::iterator iter;
    iter = m_registries.begin();
    while (iter != m_registries.end()) {
        delete iter->second;
        iter->second = 0;
        ++iter;
    }
}

void RegistryRegistry::add(std::string filepath, WriteLogRegistry* registry)
{
    util::LockGuard lock(m_mutex);
    m_registries[filepath] = registry;
}


void RegistryRegistry::remove(std::string filepath)
{
    util::LockGuard lock(m_mutex);
    m_registries.erase(filepath);
}


RegistryRegistry globalRegistry;





class TransactLogRegistryImpl : public SharedGroup::TransactLogRegistry {
private:
    WriteLogRegistry* m_registry;
    int m_interest_key;
public:
    TransactLogRegistryImpl(WriteLogRegistry* registry) : m_registry(registry)
    {
        m_interest_key = registry->register_interest();
    }

    ~TransactLogRegistryImpl()
    {
        m_registry->unregister_interest(m_interest_key);
        m_registry = 0;
    }

    virtual void get_commit_entries(uint_fast64_t from_version, uint_fast64_t to_version,
                                    BinaryData* logs_buffer) TIGHTDB_NOEXCEPT
    {
        m_registry->get_commit_entries(from_version, to_version, logs_buffer);
    }

    virtual void release_commit_entries(uint_fast64_t to_version) TIGHTDB_NOEXCEPT
    {
        m_registry->release_commit_entries(m_interest_key, to_version);
    }
};




SharedGroup::TransactLogRegistry* getWriteLogs(std::string filepath)
{
    return new TransactLogRegistryImpl(globalRegistry.get(filepath));
}







class WriteLogCollector : public Replication
{
public:
    WriteLogCollector(std::string database_name, 
		      WriteLogRegistry* registry);
    ~WriteLogCollector() TIGHTDB_NOEXCEPT {};
    std::string do_get_database_path() TIGHTDB_OVERRIDE { return m_database_name; }
    void do_begin_write_transact(SharedGroup& sg) TIGHTDB_OVERRIDE;
    version_type do_commit_write_transact(SharedGroup& sg, version_type orig_version) TIGHTDB_OVERRIDE;
    void do_rollback_write_transact(SharedGroup& sg) TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE;
    void do_interrupt() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {};
    void do_clear_interrupt() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {};
    void do_transact_log_reserve(std::size_t sz) TIGHTDB_OVERRIDE;
    void do_transact_log_append(const char* data, std::size_t size) TIGHTDB_OVERRIDE;
    void transact_log_reserve(std::size_t n) TIGHTDB_OVERRIDE;
protected:
    std::string m_database_name;
    util::Buffer<char> m_transact_log_buffer;
    WriteLogRegistry* m_registry;
};


Replication* makeWriteLogCollector(std::string database_name)
{
    WriteLogRegistry* registry = globalRegistry.get(database_name);
    return  new WriteLogCollector(database_name, registry);
}








void WriteLogCollector::do_begin_write_transact(SharedGroup& sg) TIGHTDB_OVERRIDE
{
    static_cast<void>(sg);
    m_transact_log_free_begin = m_transact_log_buffer.data();
    m_transact_log_free_end   = m_transact_log_free_begin + m_transact_log_buffer.size();
}


WriteLogCollector::version_type 
WriteLogCollector::do_commit_write_transact(SharedGroup& sg, 
	WriteLogCollector::version_type orig_version) TIGHTDB_OVERRIDE
{
    static_cast<void>(sg);
    char* data     = m_transact_log_buffer.release();
    std::size_t sz = m_transact_log_free_begin - data;
    version_type new_version = orig_version + 1;
    m_registry->add_commit(new_version, data, sz);
    return new_version;
}


void WriteLogCollector::do_rollback_write_transact(SharedGroup& sg) TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE
{
    // not used in this setting
    static_cast<void>(sg);
}


void WriteLogCollector::do_transact_log_reserve(std::size_t sz) TIGHTDB_OVERRIDE
{
    transact_log_reserve(sz);
}


void WriteLogCollector::do_transact_log_append(const char* data, std::size_t size) TIGHTDB_OVERRIDE
{
    transact_log_reserve(size);
    m_transact_log_free_begin = std::copy(data, data+size, m_transact_log_free_begin);
}


void WriteLogCollector::transact_log_reserve(std::size_t n) TIGHTDB_OVERRIDE
{
    char* data = m_transact_log_buffer.data();
    std::size_t size = m_transact_log_free_begin - data;
    m_transact_log_buffer.reserve_extra(size, n);
    data = m_transact_log_buffer.data();
    m_transact_log_free_begin = data + size;
    m_transact_log_free_end = data + m_transact_log_buffer.size();
}


WriteLogCollector::WriteLogCollector(std::string database_name, WriteLogRegistry* registry)
{
    m_database_name = database_name;
    m_registry = registry;
}

} // namespace tightdb
