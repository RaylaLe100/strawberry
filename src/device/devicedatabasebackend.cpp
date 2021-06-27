/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QIODevice>
#include <QFile>
#include <QByteArray>
#include <QString>
#include <QSqlDatabase>

#include "core/database.h"
#include "core/sqlquery.h"
#include "core/scopedtransaction.h"
#include "devicedatabasebackend.h"

const int DeviceDatabaseBackend::kDeviceSchemaVersion = 3;

DeviceDatabaseBackend::DeviceDatabaseBackend(QObject *parent)
    : QObject(parent),
      db_(nullptr),
      original_thread_(nullptr) {

  original_thread_ = thread();

}

void DeviceDatabaseBackend::Init(Database *db) { db_ = db; }

void DeviceDatabaseBackend::Close() {

  if (db_) {
    QMutexLocker l(db_->Mutex());
    db_->Close();
  }

}

void DeviceDatabaseBackend::ExitAsync() {
  QMetaObject::invokeMethod(this, "Exit", Qt::QueuedConnection);
}

void DeviceDatabaseBackend::Exit() {

  Q_ASSERT(QThread::currentThread() == thread());
  Close();
  moveToThread(original_thread_);
  emit ExitFinished();

}

DeviceDatabaseBackend::DeviceList DeviceDatabaseBackend::GetAllDevices() {

  DeviceList ret;
  DeviceList old_devices;

  {
    QMutexLocker l(db_->Mutex());
    QSqlDatabase db(db_->Connect());
    SqlQuery q(db);
    q.prepare("SELECT ROWID, unique_id, friendly_name, size, icon, schema_version, transcode_mode, transcode_format FROM devices");
    if (!q.Exec()) {
      db_->ReportErrors(q);
      return ret;
    }

    while (q.next()) {
      Device dev;
      dev.id_ = q.value(0).toInt();
      dev.unique_id_ = q.value(1).toString();
      dev.friendly_name_ = q.value(2).toString();
      dev.size_ = q.value(3).toLongLong();
      dev.icon_name_ = q.value(4).toString();
      int schema_version = q.value(5).toInt();
      dev.transcode_mode_ = static_cast<MusicStorage::TranscodeMode>(q.value(6).toInt());
      dev.transcode_format_ = static_cast<Song::FileType>(q.value(7).toInt());
      if (schema_version < kDeviceSchemaVersion) {  // Device is using old schema, drop it.
        old_devices << dev;
      }
      else {
        ret << dev;
      }
    }
  }

  for (const Device &dev : old_devices) {
    RemoveDevice(dev.id_);
  }

  Close();

  return ret;

}

int DeviceDatabaseBackend::AddDevice(const Device &device) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  ScopedTransaction t(&db);

  // Insert the device into the devices table
  SqlQuery q(db);
  q.prepare("INSERT INTO devices (unique_id, friendly_name, size, icon, transcode_mode, transcode_format) VALUES (:unique_id, :friendly_name, :size, :icon, :transcode_mode, :transcode_format)");
  q.BindValue(":unique_id", device.unique_id_);
  q.BindValue(":friendly_name", device.friendly_name_);
  q.BindValue(":size", device.size_);
  q.BindValue(":icon", device.icon_name_);
  q.BindValue(":transcode_mode", device.transcode_mode_);
  q.BindValue(":transcode_format", device.transcode_format_);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return -1;
  }
  int id = q.lastInsertId().toInt();

  // Create the songs tables for the device
  QString filename(":/schema/device-schema.sql");
  QFile schema_file(filename);
  if (!schema_file.open(QIODevice::ReadOnly)) {
    qFatal("Couldn't open schema file %s: %s", filename.toUtf8().constData(), schema_file.errorString().toUtf8().constData());
  }
  QString schema = QString::fromUtf8(schema_file.readAll());
  schema.replace("%deviceid", QString::number(id));

  db_->ExecSchemaCommands(db, schema, 0, true);

  t.Commit();

  return id;

}

void DeviceDatabaseBackend::RemoveDevice(const int id) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  ScopedTransaction t(&db);

  // Remove the device from the devices table
  SqlQuery q(db);
  q.prepare("DELETE FROM devices WHERE ROWID=:id");
  q.BindValue(":id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
    return;
  }

  // Remove the songs tables for the device
  db.exec(QString("DROP TABLE device_%1_songs").arg(id));
  db.exec(QString("DROP TABLE IF EXISTS device_%1_fts").arg(id));
  db.exec(QString("DROP TABLE device_%1_directories").arg(id));
  db.exec(QString("DROP TABLE device_%1_subdirectories").arg(id));

  t.Commit();

}

void DeviceDatabaseBackend::SetDeviceOptions(const int id, const QString &friendly_name, const QString &icon_name, const MusicStorage::TranscodeMode mode, const Song::FileType format) {

  QMutexLocker l(db_->Mutex());
  QSqlDatabase db(db_->Connect());

  SqlQuery q(db);
  q.prepare(
      "UPDATE devices"
      " SET friendly_name=:friendly_name,"
      "     icon=:icon_name,"
      "     transcode_mode=:transcode_mode,"
      "     transcode_format=:transcode_format"
      " WHERE ROWID=:id");
  q.BindValue(":friendly_name", friendly_name);
  q.BindValue(":icon_name", icon_name);
  q.BindValue(":transcode_mode", mode);
  q.BindValue(":transcode_format", format);
  q.BindValue(":id", id);
  if (!q.Exec()) {
    db_->ReportErrors(q);
  }

}
