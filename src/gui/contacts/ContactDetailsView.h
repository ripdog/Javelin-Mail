#pragma once

#include "jmap/cache/ContactReader.h"
#include "jmap/contacts/ContactTypes.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <vector>

class QLabel;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace javelin::gui::contacts
{
    class ContactDetailsView final
    {
      public:
        ContactDetailsView(QLabel& name, QLabel& location, QToolButton& star, QLabel& photo,
                           QLabel& editorPhoto, QWidget& cardContainer, QVBoxLayout& cardLayout,
                           javelin::jmap::cache::ContactReader& repository,
                           const std::vector<javelin::jmap::cache::ContactAccount>& accounts,
                           std::function<QString()> currentAccountId,
                           std::function<void(QString, QString, QString)> composeMail,
                           std::function<void(QString, QString)> searchMailFrom);

        void showIdentity(const javelin::jmap::contacts::ContactSummary& contact,
                          const QString& locationText, bool starredEnabled);
        void populateCards(const javelin::jmap::contacts::ContactSummary& contact);
        void clearPhoto();
        void showPhoto(const QByteArray& payload);

      private:
        QLabel& m_name;
        QLabel& m_location;
        QToolButton& m_star;
        QLabel& m_photo;
        QLabel& m_editorPhoto;
        QWidget& m_cardContainer;
        QVBoxLayout& m_cardLayout;
        javelin::jmap::cache::ContactReader& m_repository;
        const std::vector<javelin::jmap::cache::ContactAccount>& m_accounts;
        std::function<QString()> m_currentAccountId;
        std::function<void(QString, QString, QString)> m_composeMail;
        std::function<void(QString, QString)> m_searchMailFrom;
    };
} // namespace javelin::gui::contacts
