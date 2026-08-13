#include "gui/contacts/ContactPhotoController.h"

#include "jmap/contacts/ContactTypes.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <utility>
#include <variant>

namespace javelin::gui::contacts
{
    ContactPhotoController::ContactPhotoController(
        javelin::app::ContactCommandPort& commandPort, QObject& context,
        std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
        std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage)
        : m_commandPort(commandPort), m_context(context),
          m_ownerAccountId(std::move(ownerAccountId)), m_setBusy(std::move(setBusy)),
          m_statusMessage(std::move(statusMessage))
    {
    }

    void ContactPhotoController::upload(const std::string& accountId, QByteArray payload,
                                        std::string mediaType,
                                        std::function<std::string()> currentDocument,
                                        std::function<void(std::string)> setDocument,
                                        std::function<void(const QByteArray&)> showPhoto,
                                        std::function<void(bool)> setRemoveEnabled)
    {
        const auto owner = m_ownerAccountId(accountId).value_or(std::string{});
        m_setBusy(true);
        auto task =
            m_commandPort.uploadContactMedia(owner, accountId, payload, std::move(mediaType));
        QCoro::connect(
            std::move(task), &m_context,
            [this, payload = std::move(payload), currentDocument = std::move(currentDocument),
             setDocument = std::move(setDocument), showPhoto = std::move(showPhoto),
             setRemoveEnabled =
                 std::move(setRemoveEnabled)](javelin::jmap::contacts::ContactUploadResult result)
            {
                m_setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_statusMessage(error->message, 10000);
                    return;
                }
                const auto& media = std::get<javelin::jmap::contacts::UploadedContactMedia>(result);
                const auto document = javelin::jmap::contacts::setContactPhoto(
                    currentDocument(), media.blobId, media.mediaType);
                if (const auto* message = std::get_if<std::string_view>(&document))
                {
                    m_statusMessage(
                        QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())),
                        10000);
                    return;
                }
                setDocument(std::get<std::string>(document));
                showPhoto(payload);
                setRemoveEnabled(true);
                m_statusMessage(i18n("Photo uploaded; save the contact to apply it."), 5000);
            });
    }

    std::variant<std::string, QString>
    ContactPhotoController::remove(const std::string& document, std::function<void()> clearPhoto,
                                   std::function<void(bool)> setRemoveEnabled) const
    {
        const auto updated = javelin::jmap::contacts::removeContactPhoto(document);
        if (const auto* message = std::get_if<std::string_view>(&updated))
        {
            return QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size()));
        }
        clearPhoto();
        setRemoveEnabled(false);
        return std::get<std::string>(updated);
    }

    void ContactPhotoController::show(const javelin::jmap::contacts::ContactSummary& contact,
                                      std::function<void()> clearPhoto,
                                      std::function<void(const QByteArray&)> showPhoto,
                                      std::function<bool(const std::string&)> stillSelected)
    {
        clearPhoto();
        const auto photo = javelin::jmap::contacts::contactPhoto(contact.document);
        if (!photo.has_value())
            return;
        if (photo->uri.has_value() && photo->uri->starts_with("data:"))
        {
            const QByteArray uri = QByteArray::fromStdString(*photo->uri);
            const auto separator = uri.indexOf(',');
            if (separator > 0)
            {
                const auto metadata = uri.first(separator);
                const auto payload = uri.sliced(separator + 1);
                showPhoto(metadata.contains(";base64") ? QByteArray::fromBase64(payload)
                                                       : QByteArray::fromPercentEncoding(payload));
            }
            return;
        }
        if (!photo->blobId.has_value())
            return;

        const auto accountId = contact.accountId;
        const auto contactId = contact.id;
        auto task = m_commandPort.downloadContactMedia(
            m_ownerAccountId(accountId).value_or(std::string{}), accountId, *photo->blobId,
            photo->mediaType.value_or("application/octet-stream"));
        QCoro::connect(
            std::move(task), &m_context,
            [contactId, showPhoto = std::move(showPhoto), stillSelected = std::move(stillSelected)](
                javelin::jmap::contacts::ContactDownloadResult result)
            {
                if (!stillSelected(contactId))
                    return;
                if (const auto* media =
                        std::get_if<javelin::jmap::contacts::DownloadedContactMedia>(&result))
                    showPhoto(media->data);
            });
    }
} // namespace javelin::gui::contacts
