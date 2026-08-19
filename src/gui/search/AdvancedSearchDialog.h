#pragma once

#include "jmap/search/EmailSearch.h"

#include <QDialog>

class QLineEdit;
class QWidget;

namespace javelin::gui::search
{

    class AdvancedSearchDialog : public QDialog
    {
        Q_OBJECT

      public:
        explicit AdvancedSearchDialog(QWidget* parent = nullptr);
        AdvancedSearchDialog(const javelin::jmap::search::EmailSearchCriteria& criteria,
                             QWidget* parent);

        [[nodiscard]] javelin::jmap::search::EmailSearchCriteria criteria() const;

      private:
        QLineEdit* m_textEdit = nullptr;
        QLineEdit* m_withEdit = nullptr;
        QLineEdit* m_fromEdit = nullptr;
        QLineEdit* m_toEdit = nullptr;
        QLineEdit* m_ccEdit = nullptr;
        QLineEdit* m_bccEdit = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        QLineEdit* m_bodyEdit = nullptr;
    };

} // namespace javelin::gui::search
