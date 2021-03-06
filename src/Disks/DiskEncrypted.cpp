#include <Disks/DiskEncrypted.h>

#if USE_SSL
#include <Disks/DiskFactory.h>
#include <IO/FileEncryptionCommon.h>
#include <IO/ReadBufferFromEncryptedFile.h>
#include <IO/WriteBufferFromEncryptedFile.h>
#include <boost/algorithm/hex.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int INCORRECT_DISK_INDEX;
}

namespace
{
    using DiskEncryptedPtr = std::shared_ptr<DiskEncrypted>;
    using namespace FileEncryption;

    String unhexKey(const String & hex, const String & disk_name)
    {
        try
        {
            return boost::algorithm::unhex(hex);
        }
        catch (const std::exception &)
        {
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cannot read key_hex for disk {}, check for valid characters [0-9a-fA-F] and length", disk_name);
        }
    }

    struct DiskEncryptedSettings
    {
        String key;
        DiskPtr wrapped_disk;
        String path_on_wrapped_disk;

        DiskEncryptedSettings(
            const String & disk_name, const Poco::Util::AbstractConfiguration & config, const String & config_prefix, const DisksMap & map)
        {
            String wrapped_disk_name = config.getString(config_prefix + ".disk", "");
            if (wrapped_disk_name.empty())
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Name of the wrapped disk must not be empty. An encrypted disk is a wrapper over another disk. "
                    "Disk {}", disk_name);

            key = config.getString(config_prefix + ".key", "");
            String key_hex = config.getString(config_prefix + ".key_hex", "");
            if (!key.empty() && !key_hex.empty())
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Both 'key' and 'key_hex' are specified. There should be only one. Disk {}", disk_name);

            if (!key_hex.empty())
            {
                assert(key.empty());
                key = unhexKey(key_hex, disk_name);
            }

            if (key.empty())
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Key of the encrypted disk must not be empty. Disk {}", disk_name);

            if (!FileEncryption::isKeyLengthSupported(key.length()))
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Key length is not supported, supported only keys of length 16, 24, or 32 bytes. Disk {}", disk_name);

            auto wrapped_disk_it = map.find(wrapped_disk_name);
            if (wrapped_disk_it == map.end())
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "The wrapped disk must have been announced earlier. No disk with name {}. Disk {}",
                    wrapped_disk_name, disk_name);
            wrapped_disk = wrapped_disk_it->second;

            path_on_wrapped_disk = config.getString(config_prefix + ".path", "");
        }
    };

    bool inline isSameDiskType(const IDisk & one, const IDisk & another)
    {
        return typeid(one) == typeid(another);
    }
}

class DiskEncryptedReservation : public IReservation
{
public:
    DiskEncryptedReservation(DiskEncryptedPtr disk_, std::unique_ptr<IReservation> reservation_)
        : disk(std::move(disk_)), reservation(std::move(reservation_))
    {
    }

    UInt64 getSize() const override { return reservation->getSize(); }

    DiskPtr getDisk(size_t i) const override
    {
        if (i != 0)
            throw Exception("Can't use i != 0 with single disk reservation", ErrorCodes::INCORRECT_DISK_INDEX);
        return disk;
    }

    Disks getDisks() const override { return {disk}; }

    void update(UInt64 new_size) override { reservation->update(new_size); }

private:
    DiskEncryptedPtr disk;
    std::unique_ptr<IReservation> reservation;
};

ReservationPtr DiskEncrypted::reserve(UInt64 bytes)
{
    auto reservation = delegate->reserve(bytes);
    if (!reservation)
        return {};
    return std::make_unique<DiskEncryptedReservation>(std::static_pointer_cast<DiskEncrypted>(shared_from_this()), std::move(reservation));
}

DiskEncrypted::DiskEncrypted(const String & name_, DiskPtr disk_, const String & key_, const String & path_)
    : DiskDecorator(disk_)
    , name(name_), key(key_), disk_path(path_)
{
    initialize();
}

void DiskEncrypted::initialize()
{
    disk_absolute_path = delegate->getPath() + disk_path;

    // use wrapped_disk as an EncryptedDisk store
    if (disk_path.empty())
        return;

    if (disk_path.back() != '/')
        throw Exception("Disk path must ends with '/', but '" + disk_path + "' doesn't.", ErrorCodes::BAD_ARGUMENTS);

    delegate->createDirectories(disk_path);
}

void DiskEncrypted::copy(const String & from_path, const std::shared_ptr<IDisk> & to_disk, const String & to_path)
{
    /// Check if we can copy the file without deciphering.
    if (isSameDiskType(*this, *to_disk))
    {
        /// Disk type is the same, check if the key is the same too.
        if (auto * to_encrypted_disk = typeid_cast<DiskEncrypted *>(to_disk.get()))
        {
            if (key == to_encrypted_disk->key)
            {
                /// Key is the same so we can simply copy the encrypted file.
                delegate->copy(wrappedPath(from_path), to_encrypted_disk->delegate, wrappedPath(to_path));
                return;
            }
        }
    }

    /// Copy the file through buffers with deciphering.
    copyThroughBuffers(from_path, to_disk, to_path);
}

std::unique_ptr<ReadBufferFromFileBase> DiskEncrypted::readFile(
    const String & path,
    size_t buf_size,
    size_t estimated_size,
    size_t aio_threshold,
    size_t mmap_threshold,
    MMappedFileCache * mmap_cache) const
{
    auto wrapped_path = wrappedPath(path);
    auto buffer = delegate->readFile(wrapped_path, buf_size, estimated_size, aio_threshold, mmap_threshold, mmap_cache);

    InitVector iv;
    iv.read(*buffer);
    return std::make_unique<ReadBufferFromEncryptedFile>(buf_size, std::move(buffer), key, iv);
}

std::unique_ptr<WriteBufferFromFileBase> DiskEncrypted::writeFile(const String & path, size_t buf_size, WriteMode mode)
{
    InitVector iv;
    UInt64 old_file_size = 0;
    auto wrapped_path = wrappedPath(path);

    if (mode == WriteMode::Append && exists(path) && getFileSize(path))
    {
        auto read_buffer = delegate->readFile(wrapped_path, InitVector::kSize);
        iv.read(*read_buffer);
        old_file_size = getFileSize(path);
    }
    else
        iv = InitVector::random();

    auto buffer = delegate->writeFile(wrapped_path, buf_size, mode);
    return std::make_unique<WriteBufferFromEncryptedFile>(buf_size, std::move(buffer), key, iv, old_file_size);
}


size_t DiskEncrypted::getFileSize(const String & path) const
{
    auto wrapped_path = wrappedPath(path);
    size_t size = delegate->getFileSize(wrapped_path);
    return size > InitVector::kSize ? (size - InitVector::kSize) : 0;
}

void DiskEncrypted::truncateFile(const String & path, size_t size)
{
    auto wrapped_path = wrappedPath(path);
    delegate->truncateFile(wrapped_path, size ? (size + InitVector::kSize) : 0);
}

SyncGuardPtr DiskEncrypted::getDirectorySyncGuard(const String & path) const
{
    auto wrapped_path = wrappedPath(path);
    return delegate->getDirectorySyncGuard(wrapped_path);
}

void DiskEncrypted::applyNewSettings(
    const Poco::Util::AbstractConfiguration & config,
    ContextPtr /*context*/,
    const String & config_prefix,
    const DisksMap & map)
{
    DiskEncryptedSettings settings{name, config, config_prefix, map};
    key = settings.key;
    delegate = settings.wrapped_disk;
    disk_path = settings.path_on_wrapped_disk;
    initialize();
}

void registerDiskEncrypted(DiskFactory & factory)
{
    auto creator = [](const String & name,
                      const Poco::Util::AbstractConfiguration & config,
                      const String & config_prefix,
                      ContextPtr /*context*/,
                      const DisksMap & map) -> DiskPtr
    {
        DiskEncryptedSettings settings{name, config, config_prefix, map};
        return std::make_shared<DiskEncrypted>(name, settings.wrapped_disk, settings.key, settings.path_on_wrapped_disk);
    };
    factory.registerDiskType("encrypted", creator);
}

}


#endif
