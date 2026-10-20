/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <QMessageBox>

#include "types.h"
#include "Platform.h"
#include "Config.h"
#include "main.h"

#include "Net.h"
#include "Net_PCap.h"

#include "WifiSettingsDialog.h"
#include "ui_WifiSettingsDialog.h"


#ifdef __WIN32__
#define PCAP_NAME "winpcap/npcap"
#else
#define PCAP_NAME "libpcap"
#endif

extern std::optional<melonDS::LibPCap> pcap;
extern melonDS::Net net;

WifiSettingsDialog* WifiSettingsDialog::currentDlg = nullptr;

bool WifiSettingsDialog::needsReset = false;

void NetInit();

WifiSettingsDialog::WifiSettingsDialog(QWidget* parent) : QDialog(parent), ui(new Ui::WifiSettingsDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();
    canEditSharedNetwork = emuInstance->getInstanceID() == 0;
    setWindowTitle(QString("Wifi settings - melonDS (instance %1)").arg(emuInstance->getInstanceID() + 1));
    auto& cfg = emuInstance->getGlobalConfig();
    auto& localCfg = emuInstance->getLocalConfig();
    const auto launchOverride = emuInstance->getWiiDSLaunchOverride();
    ui->chkWiiDS->setChecked(launchOverride.value_or(localCfg.GetBool("WiiDS.Automatic")));
    // The guest radio must be stopped before changing transport ownership.
    // Do not reset an unsaved game merely to change this preference.
    ui->chkWiiDS->setEnabled(!emuInstance->getEmuThread()->emuIsActive() && !launchOverride.has_value());
    if (launchOverride.has_value())
        ui->chkWiiDS->setToolTip("A command-line Wii-DS option controls this launch. Restart without the override to change the saved option.");
    ui->chkWiiDSDiagnostics->setChecked(localCfg.GetBool("WiiDS.Diagnostics"));

    if (!pcap)
        pcap = melonDS::LibPCap::New();

    haspcap = pcap.has_value();
    if (pcap)
        adapters = pcap->GetAdapters();

    ui->rbDirectMode->setText("Direct mode (requires " PCAP_NAME " and ethernet connection)");

    ui->lblAdapterMAC->setText("(none)");
    ui->lblAdapterIP->setText("(none)");

    int sel = -1;
    for (int i = 0; i < adapters.size(); i++)
    {
        melonDS::AdapterData& adapter = adapters[i];

        ui->cbxDirectAdapter->addItem(QString(adapter.FriendlyName));

        if (!strncmp(adapter.DeviceName, cfg.GetString("LAN.Device").c_str(), 128))
            sel = i;
    }
    ui->cbxDirectAdapter->setCurrentIndex(sel);

    // errrr???
    bool direct = cfg.GetBool("LAN.DirectMode");
    ui->rbDirectMode->setChecked(direct);
    ui->rbIndirectMode->setChecked(!direct);
    if (!haspcap) ui->rbDirectMode->setEnabled(false);

    updateAdapterControls();
    initialDirectMode = ui->rbDirectMode->isChecked();
    initialAdapterIndex = ui->cbxDirectAdapter->currentIndex();

    if (!canEditSharedNetwork)
    {
        ui->groupBox->setEnabled(false);
        ui->groupBox_3->setEnabled(false);
        ui->label_4->setText("Online network settings are shared by all instances. Change them from the first instance. The Dolphin Wii connection above applies to this instance only.");
    }
}

WifiSettingsDialog::~WifiSettingsDialog()
{
    if (currentDlg == this)
        closeDlg();
    delete ui;
}

void WifiSettingsDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        closeDlg();
        return;
    }

    needsReset = false;

    bool sharedNetworkChanged = false;
    if (r == QDialog::Accepted)
    {
        if (canEditSharedNetwork &&
            (ui->rbDirectMode->isChecked() != initialDirectMode ||
             ui->cbxDirectAdapter->currentIndex() != initialAdapterIndex))
        {
            auto& cfg = emuInstance->getGlobalConfig();
            int sel = ui->cbxDirectAdapter->currentIndex();
            std::string device = cfg.GetString("LAN.Device");
            if (sel != initialAdapterIndex)
                device = sel < 0 || sel >= adapters.size() ? "" : adapters[sel].DeviceName;
            const bool direct = ui->rbDirectMode->isChecked();
            sharedNetworkChanged = cfg.GetBool("LAN.DirectMode") != direct ||
                                   cfg.GetString("LAN.Device") != device;
            if (sharedNetworkChanged)
            {
                cfg.SetBool("LAN.DirectMode", direct);
                cfg.SetString("LAN.Device", device);
            }
        }

        auto& localCfg = emuInstance->getLocalConfig();
        if (ui->chkWiiDS->isEnabled())
        {
            localCfg.SetBool("WiiDS.Automatic", ui->chkWiiDS->isChecked());
            localCfg.SetBool("WiiDS.Enabled", false);
        }
        localCfg.SetBool("WiiDS.Diagnostics", ui->chkWiiDSDiagnostics->isChecked());
        emuInstance->applyWiiDSSettings();
        Config::Save();
    }

    // Local Wii-link changes (and Cancel) must not restart every instance's
    // shared online network backend.
    if (sharedNetworkChanged)
        NetInit();

    QDialog::done(r);

    closeDlg();
}

void WifiSettingsDialog::on_rbDirectMode_clicked()
{
    updateAdapterControls();
}

void WifiSettingsDialog::on_rbIndirectMode_clicked()
{
    updateAdapterControls();
}

void WifiSettingsDialog::on_cbxDirectAdapter_currentIndexChanged(int sel)
{
    if (!haspcap) return;

    if (sel < 0 || sel >= adapters.size() || adapters.empty()) return;

    melonDS::AdapterData* adapter = &adapters[sel];
    char tmp[64];

    snprintf(tmp, sizeof(tmp), "%02X:%02X:%02X:%02X:%02X:%02X",
             adapter->MAC[0], adapter->MAC[1], adapter->MAC[2],
             adapter->MAC[3], adapter->MAC[4], adapter->MAC[5]);
    ui->lblAdapterMAC->setText(QString(tmp));

    snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d",
             adapter->IP_v4[0], adapter->IP_v4[1],
             adapter->IP_v4[2], adapter->IP_v4[3]);
    ui->lblAdapterIP->setText(QString(tmp));
}

void WifiSettingsDialog::updateAdapterControls()
{
    bool enable = haspcap && ui->rbDirectMode->isChecked();

    ui->cbxDirectAdapter->setEnabled(enable);
    ui->lblAdapterMAC->setEnabled(enable);
    ui->lblAdapterIP->setEnabled(enable);
}
