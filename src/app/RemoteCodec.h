#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>

#include <glaze/glaze.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app::remote
{
    constexpr qsizetype maximumPayloadBytes = 64 * 1024 * 1024;
    constexpr std::uint32_t maximumCollectionItems = 1'000'000;

    struct CodecError
    {
        QString message;
    };

    template <typename T> using DecodeResult = std::variant<T, CodecError>;
    using EncodeResult = std::variant<QByteArray, CodecError>;

    namespace detail
    {
        template <typename T> struct IsOptional : std::false_type
        {
        };
        template <typename T> struct IsOptional<std::optional<T>> : std::true_type
        {
            using Value = T;
        };

        template <typename T> struct IsVariant : std::false_type
        {
        };
        template <typename... Ts> struct IsVariant<std::variant<Ts...>> : std::true_type
        {
        };

        template <typename T> struct IsVector : std::false_type
        {
        };
        template <typename T, typename Allocator>
        struct IsVector<std::vector<T, Allocator>> : std::true_type
        {
            using Value = T;
        };

        template <typename T> struct IsStdArray : std::false_type
        {
        };
        template <typename T, std::size_t Size>
        struct IsStdArray<std::array<T, Size>> : std::true_type
        {
            using Value = T;
            static constexpr std::size_t size = Size;
        };

        template <typename T> struct IsMap : std::false_type
        {
        };
        template <typename Key, typename Value, typename Compare, typename Allocator>
        struct IsMap<std::map<Key, Value, Compare, Allocator>> : std::true_type
        {
        };
        template <typename Key, typename Value, typename Hash, typename Equal, typename Allocator>
        struct IsMap<std::unordered_map<Key, Value, Hash, Equal, Allocator>> : std::true_type
        {
        };

        template <typename T> struct IsSet : std::false_type
        {
        };
        template <typename Value, typename Compare, typename Allocator>
        struct IsSet<std::set<Value, Compare, Allocator>> : std::true_type
        {
        };
        template <typename Value, typename Hash, typename Equal, typename Allocator>
        struct IsSet<std::unordered_set<Value, Hash, Equal, Allocator>> : std::true_type
        {
        };

        template <typename T> struct IsPair : std::false_type
        {
        };
        template <typename First, typename Second>
        struct IsPair<std::pair<First, Second>> : std::true_type
        {
        };

        template <typename T> struct IsTuple : std::false_type
        {
        };
        template <typename... Ts> struct IsTuple<std::tuple<Ts...>> : std::true_type
        {
        };

        template <typename T> struct IsDuration : std::false_type
        {
        };
        template <typename Rep, typename Period>
        struct IsDuration<std::chrono::duration<Rep, Period>> : std::true_type
        {
            using RepType = Rep;
        };

        template <typename T> struct IsTimePoint : std::false_type
        {
        };
        template <typename Clock, typename Duration>
        struct IsTimePoint<std::chrono::time_point<Clock, Duration>> : std::true_type
        {
            using DurationType = Duration;
        };

        template <typename T>
        concept QtStreamWritable =
            requires(QDataStream& stream, const T& value) { stream << value; };

        template <typename T>
        concept QtStreamReadable = requires(QDataStream& stream, T& value) { stream >> value; };

        class Writer;
        class Reader;

        template <typename T> bool write(Writer& writer, const T& value);
        template <typename T> bool read(Reader& reader, T& value);

        class Writer final
        {
          public:
            Writer() : m_stream(&m_payload, QIODeviceBase::WriteOnly)
            {
                m_stream.setVersion(QDataStream::Qt_6_6);
                m_stream.setByteOrder(QDataStream::BigEndian);
            }

            template <typename T> bool value(const T& item)
            {
                return write(*this, item);
            }

            template <typename T> bool qtValue(const T& item)
            {
                m_stream << item;
                return check();
            }

            bool rawBytes(const QByteArray& bytes)
            {
                if (bytes.size() > maximumPayloadBytes)
                    return fail(QStringLiteral("Remote payload exceeds the size limit."));
                m_stream << bytes;
                return check();
            }

            bool count(const std::size_t count)
            {
                if (count > maximumCollectionItems ||
                    count > std::numeric_limits<std::uint32_t>::max())
                    return fail(QStringLiteral("Remote collection exceeds the item limit."));
                return qtValue(static_cast<quint32>(count));
            }

            bool fail(QString message)
            {
                if (!m_error.has_value())
                    m_error = CodecError{.message = std::move(message)};
                return false;
            }

            EncodeResult finish()
            {
                if (!check())
                    return *m_error;
                if (m_payload.size() > maximumPayloadBytes)
                    return CodecError{.message =
                                          QStringLiteral("Remote payload exceeds the size limit.")};
                return std::move(m_payload);
            }

          private:
            bool check()
            {
                if (m_error.has_value())
                    return false;
                if (m_stream.status() == QDataStream::Ok)
                    return true;
                return fail(QStringLiteral("Unable to encode remote value."));
            }

            QByteArray m_payload;
            QDataStream m_stream;
            std::optional<CodecError> m_error;
        };

        class Reader final
        {
          public:
            explicit Reader(QByteArray payload)
                : m_payload(std::move(payload)), m_stream(&m_payload, QIODeviceBase::ReadOnly)
            {
                m_stream.setVersion(QDataStream::Qt_6_6);
                m_stream.setByteOrder(QDataStream::BigEndian);
                if (m_payload.size() > maximumPayloadBytes)
                    fail(QStringLiteral("Remote payload exceeds the size limit."));
            }

            template <typename T> bool value(T& item)
            {
                return read(*this, item);
            }

            template <typename T> bool qtValue(T& item)
            {
                m_stream >> item;
                return check();
            }

            bool rawBytes(QByteArray& bytes)
            {
                m_stream >> bytes;
                if (!check())
                    return false;
                if (bytes.size() > maximumPayloadBytes)
                    return fail(QStringLiteral("Remote byte array exceeds the size limit."));
                return true;
            }

            bool count(std::uint32_t& count)
            {
                quint32 encoded = 0;
                if (!qtValue(encoded))
                    return false;
                if (encoded > maximumCollectionItems)
                    return fail(QStringLiteral("Remote collection exceeds the item limit."));
                count = encoded;
                return true;
            }

            bool fail(QString message)
            {
                if (!m_error.has_value())
                    m_error = CodecError{.message = std::move(message)};
                return false;
            }

            std::optional<CodecError> finish()
            {
                if (!check())
                    return m_error;
                if (!m_stream.atEnd())
                    return CodecError{.message =
                                          QStringLiteral("Remote payload has trailing data.")};
                return std::nullopt;
            }

          private:
            bool check()
            {
                if (m_error.has_value())
                    return false;
                if (m_stream.status() == QDataStream::Ok)
                    return true;
                return fail(QStringLiteral("Unable to decode remote value."));
            }

            QByteArray m_payload;
            QDataStream m_stream;
            std::optional<CodecError> m_error;
        };

        template <typename Variant, std::size_t Index = 0>
        bool readVariantAlternative(Reader& reader, const std::size_t requestedIndex,
                                    Variant& variant)
        {
            if constexpr (Index >= std::variant_size_v<Variant>)
            {
                return reader.fail(QStringLiteral("Remote variant index is invalid."));
            }
            else
            {
                if (requestedIndex == Index)
                {
                    using Alternative = std::variant_alternative_t<Index, Variant>;
                    Alternative value{};
                    if (!read(reader, value))
                        return false;
                    variant = std::move(value);
                    return true;
                }
                return readVariantAlternative<Variant, Index + 1>(reader, requestedIndex, variant);
            }
        }

        template <typename Tuple, std::size_t... Indices>
        bool writeTuple(Writer& writer, const Tuple& tuple, std::index_sequence<Indices...>)
        {
            return (write(writer, std::get<Indices>(tuple)) && ...);
        }

        template <typename Tuple, std::size_t... Indices>
        bool readTuple(Reader& reader, Tuple& tuple, std::index_sequence<Indices...>)
        {
            return (read(reader, std::get<Indices>(tuple)) && ...);
        }

        template <typename Value, std::size_t... Indices>
        bool writeAggregate(Writer& writer, const Value& value, std::index_sequence<Indices...>)
        {
            constexpr auto names = glz::member_names<Value>;
            auto fields = glz::to_tie(value);
            if (!writer.qtValue(static_cast<quint16>(1)) || !writer.count(sizeof...(Indices)))
                return false;
            return ((write(writer, std::string{names[Indices]}) &&
                     write(writer, glz::get<Indices>(fields))) &&
                    ...);
        }

        template <typename Value, std::size_t Index = 0>
        bool readAggregateField(Reader& reader, Value& value, const std::size_t requestedIndex)
        {
            constexpr auto fieldCount = glz::member_names<Value>.size();
            if constexpr (Index >= fieldCount)
            {
                return reader.fail(QStringLiteral("Remote aggregate field index is invalid."));
            }
            else
            {
                if (requestedIndex == Index)
                {
                    auto fields = glz::to_tie(value);
                    return read(reader, glz::get<Index>(fields));
                }
                return readAggregateField<Value, Index + 1>(reader, value, requestedIndex);
            }
        }

        template <typename Value> bool readAggregate(Reader& reader, Value& value)
        {
            constexpr auto names = glz::member_names<Value>;
            constexpr auto fieldCount = names.size();
            quint16 schemaVersion = 0;
            if (!reader.qtValue(schemaVersion) || schemaVersion != 1)
                return reader.fail(
                    QStringLiteral("Remote aggregate schema version is unsupported."));
            std::uint32_t encodedCount = 0;
            if (!reader.count(encodedCount) || encodedCount != fieldCount)
                return reader.fail(QStringLiteral("Remote aggregate field count is invalid."));

            std::array<bool, fieldCount> seen{};
            for (std::uint32_t encodedIndex = 0; encodedIndex < encodedCount; ++encodedIndex)
            {
                std::string fieldName;
                if (!read(reader, fieldName))
                    return false;
                std::optional<std::size_t> fieldIndex;
                for (std::size_t index = 0; index < fieldCount; ++index)
                {
                    if (names[index] == fieldName)
                    {
                        fieldIndex = index;
                        break;
                    }
                }
                if (!fieldIndex.has_value())
                    return reader.fail(QStringLiteral("Remote aggregate field is unknown."));
                if (seen[*fieldIndex])
                    return reader.fail(QStringLiteral("Remote aggregate field is duplicated."));
                seen[*fieldIndex] = true;
                if (!readAggregateField(reader, value, *fieldIndex))
                    return false;
            }
            for (const bool wasSeen : seen)
            {
                if (!wasSeen)
                    return reader.fail(QStringLiteral("Remote aggregate field is missing."));
            }
            return true;
        }

        template <typename T> bool write(Writer& writer, const T& value)
        {
            using Value = std::remove_cvref_t<T>;
            if constexpr (std::is_same_v<Value, std::monostate>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, std::string>)
            {
                return writer.rawBytes(QByteArray::fromStdString(value));
            }
            else if constexpr (std::is_same_v<Value, QByteArray>)
            {
                return writer.rawBytes(value);
            }
            else if constexpr (IsOptional<Value>::value)
            {
                return writer.qtValue(value.has_value()) &&
                       (!value.has_value() || write(writer, *value));
            }
            else if constexpr (IsVariant<Value>::value)
            {
                if (!writer.qtValue(static_cast<quint32>(value.index())))
                    return false;
                return std::visit([&writer](const auto& item) { return write(writer, item); },
                                  value);
            }
            else if constexpr (IsVector<Value>::value)
            {
                if (!writer.count(value.size()))
                    return false;
                for (const auto& item : value)
                {
                    if (!write(writer, item))
                        return false;
                }
                return true;
            }
            else if constexpr (IsStdArray<Value>::value)
            {
                for (const auto& item : value)
                {
                    if (!write(writer, item))
                        return false;
                }
                return true;
            }
            else if constexpr (IsMap<Value>::value)
            {
                if (!writer.count(value.size()))
                    return false;
                for (const auto& [key, item] : value)
                {
                    if (!write(writer, key) || !write(writer, item))
                        return false;
                }
                return true;
            }
            else if constexpr (IsSet<Value>::value)
            {
                if (!writer.count(value.size()))
                    return false;
                for (const auto& item : value)
                {
                    if (!write(writer, item))
                        return false;
                }
                return true;
            }
            else if constexpr (IsPair<Value>::value)
            {
                return write(writer, value.first) && write(writer, value.second);
            }
            else if constexpr (IsTuple<Value>::value)
            {
                return writeTuple(writer, value,
                                  std::make_index_sequence<std::tuple_size_v<Value>>{});
            }
            else if constexpr (IsDuration<Value>::value)
            {
                return write(writer, value.count());
            }
            else if constexpr (IsTimePoint<Value>::value)
            {
                return write(writer, value.time_since_epoch());
            }
            else if constexpr (std::is_same_v<Value, bool>)
            {
                return writer.qtValue(static_cast<quint8>(value ? 1 : 0));
            }
            else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>)
            {
                return writer.qtValue(static_cast<qint64>(value));
            }
            else if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value>)
            {
                return writer.qtValue(static_cast<quint64>(value));
            }
            else if constexpr (std::is_floating_point_v<Value>)
            {
                return writer.qtValue(static_cast<double>(value));
            }
            else if constexpr (std::is_enum_v<Value>)
            {
                return write(writer, static_cast<std::underlying_type_t<Value>>(value));
            }
            else if constexpr (QtStreamWritable<Value>)
            {
                return writer.qtValue(value);
            }
            else if constexpr (std::is_aggregate_v<Value>)
            {
                return writeAggregate(writer, value,
                                      std::make_index_sequence<glz::member_names<Value>.size()>{});
            }
            else
            {
                static_assert(sizeof(Value) == 0, "RemoteCodec cannot encode this type");
            }
        }

        template <typename T> bool read(Reader& reader, T& value)
        {
            using Value = std::remove_cvref_t<T>;
            if constexpr (std::is_same_v<Value, std::monostate>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<Value, std::string>)
            {
                QByteArray bytes;
                if (!reader.rawBytes(bytes))
                    return false;
                value = bytes.toStdString();
                return true;
            }
            else if constexpr (std::is_same_v<Value, QByteArray>)
            {
                return reader.rawBytes(value);
            }
            else if constexpr (IsOptional<Value>::value)
            {
                bool present = false;
                if (!reader.qtValue(present))
                    return false;
                if (!present)
                {
                    value.reset();
                    return true;
                }
                typename IsOptional<Value>::Value item{};
                if (!read(reader, item))
                    return false;
                value = std::move(item);
                return true;
            }
            else if constexpr (IsVariant<Value>::value)
            {
                quint32 index = 0;
                if (!reader.qtValue(index))
                    return false;
                return readVariantAlternative(reader, index, value);
            }
            else if constexpr (IsVector<Value>::value)
            {
                std::uint32_t count = 0;
                if (!reader.count(count))
                    return false;
                value.clear();
                value.reserve(count);
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    typename IsVector<Value>::Value item{};
                    if (!read(reader, item))
                        return false;
                    value.push_back(std::move(item));
                }
                return true;
            }
            else if constexpr (IsStdArray<Value>::value)
            {
                for (auto& item : value)
                {
                    if (!read(reader, item))
                        return false;
                }
                return true;
            }
            else if constexpr (IsMap<Value>::value)
            {
                std::uint32_t count = 0;
                if (!reader.count(count))
                    return false;
                value.clear();
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    typename Value::key_type key{};
                    typename Value::mapped_type item{};
                    if (!read(reader, key) || !read(reader, item))
                        return false;
                    value.emplace(std::move(key), std::move(item));
                }
                return true;
            }
            else if constexpr (IsSet<Value>::value)
            {
                std::uint32_t count = 0;
                if (!reader.count(count))
                    return false;
                value.clear();
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    typename Value::value_type item{};
                    if (!read(reader, item))
                        return false;
                    value.emplace(std::move(item));
                }
                return true;
            }
            else if constexpr (IsPair<Value>::value)
            {
                return read(reader, value.first) && read(reader, value.second);
            }
            else if constexpr (IsTuple<Value>::value)
            {
                return readTuple(reader, value,
                                 std::make_index_sequence<std::tuple_size_v<Value>>{});
            }
            else if constexpr (IsDuration<Value>::value)
            {
                typename IsDuration<Value>::RepType count{};
                if (!read(reader, count))
                    return false;
                value = Value{count};
                return true;
            }
            else if constexpr (IsTimePoint<Value>::value)
            {
                typename IsTimePoint<Value>::DurationType duration{};
                if (!read(reader, duration))
                    return false;
                value = Value{duration};
                return true;
            }
            else if constexpr (std::is_same_v<Value, bool>)
            {
                quint8 encoded = 0;
                if (!reader.qtValue(encoded) || encoded > 1)
                    return reader.fail(QStringLiteral("Remote boolean value is invalid."));
                value = encoded != 0;
                return true;
            }
            else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>)
            {
                qint64 encoded = 0;
                if (!reader.qtValue(encoded))
                    return false;
                if (encoded < static_cast<qint64>(std::numeric_limits<Value>::min()) ||
                    encoded > static_cast<qint64>(std::numeric_limits<Value>::max()))
                    return reader.fail(
                        QStringLiteral("Remote integer is outside the target range."));
                value = static_cast<Value>(encoded);
                return true;
            }
            else if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value>)
            {
                quint64 encoded = 0;
                if (!reader.qtValue(encoded))
                    return false;
                if (encoded > static_cast<quint64>(std::numeric_limits<Value>::max()))
                    return reader.fail(
                        QStringLiteral("Remote integer is outside the target range."));
                value = static_cast<Value>(encoded);
                return true;
            }
            else if constexpr (std::is_floating_point_v<Value>)
            {
                double encoded = 0.0;
                if (!reader.qtValue(encoded))
                    return false;
                value = static_cast<Value>(encoded);
                return true;
            }
            else if constexpr (std::is_enum_v<Value>)
            {
                std::underlying_type_t<Value> encoded{};
                if (!read(reader, encoded))
                    return false;
                value = static_cast<Value>(encoded);
                return true;
            }
            else if constexpr (QtStreamReadable<Value>)
            {
                return reader.qtValue(value);
            }
            else if constexpr (std::is_aggregate_v<Value>)
            {
                return readAggregate(reader, value);
            }
            else
            {
                static_assert(sizeof(Value) == 0, "RemoteCodec cannot decode this type");
            }
        }
    } // namespace detail

    template <typename... Values> EncodeResult encode(const Values&... values)
    {
        detail::Writer writer;
        if (!(writer.value(values) && ...))
            return writer.finish();
        return writer.finish();
    }

    template <typename... Values>
    DecodeResult<std::tuple<Values...>> decode(const QByteArray& payload)
    {
        detail::Reader reader{payload};
        std::tuple<Values...> values;
        if (!reader.value(values))
        {
            const auto error = reader.finish();
            return error.value_or(
                CodecError{.message = QStringLiteral("Unable to decode remote payload.")});
        }
        if (const auto error = reader.finish())
            return *error;
        return values;
    }

    template <typename Value> DecodeResult<Value> decodeValue(const QByteArray& payload)
    {
        const auto decoded = decode<Value>(payload);
        if (const auto* error = std::get_if<CodecError>(&decoded))
            return *error;
        return std::get<0>(std::get<std::tuple<Value>>(decoded));
    }

    template <std::uint16_t SchemaVersion, typename... Values>
    EncodeResult encodeVersioned(const Values&... values)
    {
        return encode(static_cast<std::uint16_t>(SchemaVersion), values...);
    }

    template <std::uint16_t SchemaVersion, typename... Values>
    DecodeResult<std::tuple<Values...>> decodeVersioned(const QByteArray& payload)
    {
        auto decoded = decode<std::uint16_t, Values...>(payload);
        if (const auto* error = std::get_if<CodecError>(&decoded))
            return *error;
        auto values = std::get<std::tuple<std::uint16_t, Values...>>(std::move(decoded));
        if (std::get<0>(values) != SchemaVersion)
            return CodecError{.message =
                                  QStringLiteral("Remote action schema version is unsupported.")};
        return std::apply([]<typename Version, typename... Rest>(Version&&, Rest&&... rest)
                          { return std::tuple<Values...>{std::forward<Rest>(rest)...}; },
                          std::move(values));
    }

    template <std::uint16_t SchemaVersion, typename Value>
    DecodeResult<Value> decodeVersionedValue(const QByteArray& payload)
    {
        auto decoded = decodeVersioned<SchemaVersion, Value>(payload);
        if (const auto* error = std::get_if<CodecError>(&decoded))
            return *error;
        return std::get<0>(std::get<std::tuple<Value>>(std::move(decoded)));
    }

    namespace detail
    {
        template <std::uint16_t SchemaVersion, typename Tuple> struct VersionedTupleDecoder;

        template <std::uint16_t SchemaVersion, typename... Values>
        struct VersionedTupleDecoder<SchemaVersion, std::tuple<Values...>>
        {
            [[nodiscard]] static DecodeResult<std::tuple<Values...>>
            decode(const QByteArray& payload)
            {
                return remote::decodeVersioned<SchemaVersion, Values...>(payload);
            }
        };
    } // namespace detail

    template <std::uint16_t SchemaVersion, typename Tuple>
    DecodeResult<Tuple> decodeVersionedTuple(const QByteArray& payload)
    {
        return detail::VersionedTupleDecoder<SchemaVersion, Tuple>::decode(payload);
    }
} // namespace javelin::app::remote
