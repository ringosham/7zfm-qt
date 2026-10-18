// PasswordDialog.h -- port of upstream FileManager/PasswordDialog.

#ifndef Z7QT_PASSWORD_DIALOG_H
#define Z7QT_PASSWORD_DIALOG_H

#include <QDialog>

class QCheckBox;
class QLineEdit;

class PasswordDialog : public QDialog
{
  Q_OBJECT
public:
  explicit PasswordDialog(QWidget *parent);
  QString Password() const;
  void accept() override;
private:
  QLineEdit *_edit;
  QCheckBox *_show;
};

#endif
