#!/usr/bin/env python3
#-*- coding:utf-8 -*-
import numpy as np
from PyQt5 import QtCore, QtGui, QtWidgets
import datetime, time, sys, os, argparse
from cerebus import cbpy

INSTANCE = 0
LOG_SAVE_PATH = os.getcwd()
DEFAULT_THRESH_SD = 3.0
NEWLINE_CODE = '\n'
LOG_TIMER_INTERVAL = 1000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-v', default=False, action='store_true')
    args = parser.parse_args()
    app = QtWidgets.QApplication(sys.argv)
    mainwin = MainWindow(v_continuous=args.v)
    mainwin.show()
    sys.exit(app.exec_())


class ExtensionCommentParser(object):
    def __init__(self):
        self.com_prefix = 'plugin'
        self.comid_dict = {}
        self.comid_dict['process_enable'] = '128'
        self.comid_dict['set_sig_ch'] = '127'
        self.comid_dict['set_ref_ch'] = '126'
        self.comid_dict['set_mask_ch'] = '125'
        self.comid_dict['set_thresh_sd'] = '124'
        self.comid_dict['update_params'] = '123'
        self.comid_dict['chmode_mask'] = '122'
        self.comid_dict['chmode_control'] = '121'
        self.comid_dict['chmode_ref'] = '120'
        self.comid_dict['show_settings'] = '119'
        self.labelch_dict = {}

    def set_labelch_dict(self, d):
        self.labelch_dict = d

    def process_enable(self, val):
        val = '1' if val == True else '0'
        return ';'.join([self.com_prefix, self.comid_dict['process_enable'], val])

    def set_sig_ch(self, chlabel):
        if not chlabel in self.labelch_dict: return 'dummy_comment'
        ch = str(self.labelch_dict[chlabel])
        return ';'.join([self.com_prefix, self.comid_dict['set_sig_ch'], str(ch)])

    def set_ref_ch(self, chlabel):
        if not chlabel in self.labelch_dict: return 'dummy_comment'
        ch = str(self.labelch_dict[chlabel])
        return ';'.join([self.com_prefix, self.comid_dict['set_ref_ch'], str(ch)])

    def set_mask_ch(self, chlabel):
        if not chlabel in self.labelch_dict: return 'dummy_comment'
        ch = str(self.labelch_dict[chlabel])
        return ';'.join([self.com_prefix, self.comid_dict['set_mask_ch'], str(ch)])

    def set_thresh_sd(self, thresh_sd):
        val = (int(thresh_sd//1) << 16) + int((thresh_sd - thresh_sd//1)*10000)
        return ';'.join([self.com_prefix, self.comid_dict['set_thresh_sd'], str(val)])

    def update_params(self):
        return ';'.join([self.com_prefix, self.comid_dict['update_params'], '1'])

    def chmode_mask(self, val):
        val = '1' if val == True else '0'
        return ';'.join([self.com_prefix, self.comid_dict['chmode_mask'], val])

    def chmode_control(self, val):
        val = '1' if val == True else '0'
        return ';'.join([self.com_prefix, self.comid_dict['chmode_control'], val])

    def chmode_ref(self, val):
        val = '1' if val == True else '0'
        return ';'.join([self.com_prefix, self.comid_dict['chmode_ref'], val])

    def show_settings(self):
        return ';'.join([self.com_prefix, self.comid_dict['show_settings'], '1'])



class MainWindow(QtWidgets.QWidget):
    def __init__(self, v_continuous=False):
        super(MainWindow, self).__init__()
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose, True)

        # interfce for cbpy
        self.continuous = vContinuous() if v_continuous else rContinuous()
        self.com_parser = ExtensionCommentParser()
        self.com_parser.set_labelch_dict(self.continuous.get_labelch_dict())

        # logger
        self.logger = Logger()
        self.log_timer = QtCore.QTimer()
        self.log_timer.timeout.connect(self.log_timer_callback)
        self.log_timer.start(LOG_TIMER_INTERVAL)

        self.initUI()

        self.init_chlabels()

    def log_timer_callback(self):
        self.logger.set_data(self.continuous.get_comment())

    @QtCore.pyqtSlot()
    def le_callback(self):
        source = self.sender()
        if source is self.thresh_le:
            try:
                thresh_val = float(source.text())
            except:
                thresh_val = DEFAULT_THRESH_SD
            self.thresh_le.setText(str(thresh_val))
            self.continuous.send_comment(self.com_parser.set_thresh_sd(thresh_val))

    @QtCore.pyqtSlot(int)
    def selectionchange(self, i):
        source = self.sender()
        if source is self.sigchan_cb:
            self.continuous.send_comment(self.com_parser.set_sig_ch(self.sigchan_cb.currentText()))
        elif source is self.refchan_cb:
            self.continuous.send_comment(self.com_parser.set_ref_ch(self.refchan_cb.currentText()))
        elif source is self.maskchan_cb:
            self.continuous.send_comment(self.com_parser.set_mask_ch(self.maskchan_cb.currentText()))

    @QtCore.pyqtSlot(bool)
    def button_callback(self, pressed):
        source = self.sender()
        if source is self.record_button:
            if self.record_button.isChecked():
                self.logger.start_recording()
            else:
                self.logger.stop_recording()
        elif source is self.fbenable_button:
            if self.fbenable_button.isChecked():
                self.continuous.send_comment(self.com_parser.process_enable(True))
            else:
                self.continuous.send_comment(self.com_parser.process_enable(False))
        elif source is self.update_params_button:
            self.continuous.send_comment(self.com_parser.update_params())
        elif source is self.print_settings_button:
            self.continuous.send_comment(self.com_parser.show_settings())
        elif source is self.update_chlabels_button:
            self.init_chlabels()
        elif source is self.folder_path_button:
            save_folder_path = QtWidgets.QFileDialog.getExistingDirectory(self, 'Open Directory', os.path.expanduser('~'))
            save_folder_path = str(save_folder_path)
            if save_folder_path == '':
                save_folder_path = os.getcwd()
            self.folder_path_le.setText(save_folder_path)

    def init_chlabels(self):
        self.continuous.init_ch_idx()
        self.com_parser.set_labelch_dict(self.continuous.get_labelch_dict())
        self.sigchan_cb.clear()
        self.sigchan_cb.addItems(self.continuous.get_label_list())
        self.refchan_cb.clear()
        self.refchan_cb.addItems(self.continuous.get_label_list())
        self.maskchan_cb.clear()
        self.maskchan_cb.addItems(self.continuous.get_label_list())

    def cb_callback(self, state):
        source = self.sender()
        if source is self.mask_mode_cb:
            self.continuous.send_comment(self.com_parser.chmode_mask(state==QtCore.Qt.Checked))
        elif source is self.control_mode_cb:
            self.continuous.send_comment(self.com_parser.chmode_control(state==QtCore.Qt.Checked))
        elif source is self.ref_mode_cb:
            self.continuous.send_comment(self.com_parser.chmode_ref(state==QtCore.Qt.Checked))

    def initUI(self):
        self.setWindowTitle('Config')
        # # button
        self.record_button = QtWidgets.QPushButton('Record', self)
        self.record_button.setStyleSheet('background-color: red; color: white')
        self.record_button.setCheckable(True)
        self.record_button.clicked[bool].connect(self.button_callback)
        self.fbenable_button = QtWidgets.QPushButton('Stim Enable', self)
        self.fbenable_button.setStyleSheet('background-color: yellow; color: black')
        self.fbenable_button.setCheckable(True)
        self.fbenable_button.clicked[bool].connect(self.button_callback)
        self.update_params_button = QtWidgets.QPushButton('Update params', self)
        self.update_params_button.clicked[bool].connect(self.button_callback)
        self.print_settings_button = QtWidgets.QPushButton('Print settings', self)
        self.print_settings_button.clicked[bool].connect(self.button_callback)
        self.update_chlabels_button = QtWidgets.QPushButton('Update chlabels', self)
        self.update_chlabels_button.clicked[bool].connect(self.button_callback)
        self.folder_path_button = QtWidgets.QPushButton('...', self)
        self.folder_path_button.clicked[bool].connect(self.button_callback)

        # select ch
        self.sigchan_cb = QtWidgets.QComboBox()
        self.sigchan_cb.addItems(self.continuous.get_label_list())
        self.sigchan_cb.currentIndexChanged.connect(self.selectionchange)
        self.refchan_cb = QtWidgets.QComboBox()
        self.refchan_cb.addItems(self.continuous.get_label_list())
        self.refchan_cb.currentIndexChanged.connect(self.selectionchange)
        self.maskchan_cb = QtWidgets.QComboBox()
        self.maskchan_cb.addItems(self.continuous.get_label_list())
        self.maskchan_cb.currentIndexChanged.connect(self.selectionchange)

        # folder path
        self.folder_path_le = QtWidgets.QLineEdit(self)
        self.folder_path_le.setText(LOG_SAVE_PATH)
        self.folder_path_le.returnPressed.connect(self.le_callback)
        self.folder_path_le.setReadOnly(True)

        # thresh sd
        self.thresh_le = QtWidgets.QLineEdit(self)
        self.thresh_le.setText(str(DEFAULT_THRESH_SD))
        self.thresh_le.returnPressed.connect(self.le_callback)

        # check box
        self.mask_mode_cb = QtWidgets.QCheckBox('mask', self)
        self.mask_mode_cb.stateChanged.connect(self.cb_callback)
        self.ref_mode_cb = QtWidgets.QCheckBox('ref', self)
        self.ref_mode_cb.stateChanged.connect(self.cb_callback)
        self.control_mode_cb = QtWidgets.QCheckBox('control', self)
        self.control_mode_cb.stateChanged.connect(self.cb_callback)

        vbox = QtWidgets.QVBoxLayout()
        hbox1 = QtWidgets.QHBoxLayout()
        hbox1.addWidget(self.record_button)
        hbox1.addStretch()
        hbox1.addWidget(self.fbenable_button)
        hbox1.addStretch()
        vbox.addLayout(hbox1)
        hbox2 = QtWidgets.QHBoxLayout()
        hbox2.addStretch()
        fpath_flo = QtWidgets.QFormLayout()
        fpath_flo.addRow('Save path:', self.folder_path_le)
        hbox2.addLayout(fpath_flo)
        hbox2.addWidget(self.folder_path_button)
        vbox.addLayout(hbox2)
        hbox3 = QtWidgets.QHBoxLayout()
        hbox3.addStretch()
        sigchan_flo = QtWidgets.QFormLayout()
        sigchan_flo.addRow('signal:', self.sigchan_cb)
        refchan_flo = QtWidgets.QFormLayout()
        refchan_flo.addRow('ref:', self.refchan_cb)
        maskchan_flo = QtWidgets.QFormLayout()
        maskchan_flo.addRow('mask:', self.maskchan_cb)
        hbox3.addLayout(sigchan_flo)
        hbox3.addLayout(refchan_flo)
        hbox3.addLayout(maskchan_flo)
        vbox.addLayout(hbox3)
        hbox4 = QtWidgets.QHBoxLayout()
        hbox4.addStretch()
        hbox4.addWidget(self.control_mode_cb)
        hbox4.addWidget(self.ref_mode_cb)
        hbox4.addWidget(self.mask_mode_cb)
        vbox.addLayout(hbox4)
        hbox5 = QtWidgets.QHBoxLayout()
        hbox5.addStretch()
        thresh_flo = QtWidgets.QFormLayout()
        thresh_flo.addRow('Thresh SD:', self.thresh_le)
        hbox5.addLayout(thresh_flo)
        hbox5.addWidget(self.update_params_button)
        hbox5.addWidget(self.print_settings_button)
        hbox5.addWidget(self.update_chlabels_button)
        vbox.addLayout(hbox5)
        self.setLayout(vbox)

    def closeEvent(self, event):
        #メッセージ画面の設定いろいろ
        reply = QtWidgets.QMessageBox.question(self, 'Message',
            "Are you sure to quit?", QtWidgets.QMessageBox.Yes | 
            QtWidgets.QMessageBox.No, QtWidgets.QMessageBox.No)

        if reply == QtWidgets.QMessageBox.Yes:
            self.log_timer.stop()
            event.accept()
        else:
            event.ignore()


class rContinuous(object):
    def __init__(self):
        self.label_ch_dict = {}
        self.n_ch = 0
        self.init()
        self.init_ch_idx()

    def init(self):
        res, connection = cbpy.open(instance=INSTANCE,
                                    connection='default')
        print('#connection', connection)
        if res != 0:
            print('#ERROR: cbSdkOpen')
            return 1

        res, reset = cbpy.trial_config(instance=INSTANCE,
                                       reset=True,
                                       buffer_parameter={'continuous_length':20, 'comment_length':30},
                                       range_parameter={},
                                       noevent=True,
                                       nocontinuous=False,
                                       nocomment=False)
        if res != 0:
            print('#ERROR: cbSdkSetTrialConfig')
            return 1

        return 0


    def init_ch_idx(self):
        time.sleep(0.2)
        res, trial, _ = cbpy.trial_continuous(instance=INSTANCE,
                                           reset=False)
        self.ch_idx_list = []
        if len(trial) == 0:
            sys.stderr.write('# Ch label initialize error!!')
            return -1
        ch_idx_list = [item[0] for item in trial]
        for ch in ch_idx_list:
            res, chaninfo = cbpy.get_channel_config(channel=ch, instance=INSTANCE)
            if res != 0:
                sys.stderr.write('# Ch label initialize error!!')
                return -1
            self.label_ch_dict[chaninfo['label']] = ch
        self.n_ch = len(self.label_ch_dict)
        return 0

    def get_ch_idx(self, ch_label):
        if ch_label in self.label_ch_dict.keys():
            return self.label_ch_dict[ch_label]
        else:
            return None

    def get_label_list(self):
        return self.label_ch_dict.keys()

    def get_labelch_dict(self):
        return self.label_ch_dict

    def send_comment(self, cmt):
        cbpy.set_comment(cmt, instance=INSTANCE)

    def get_comment(self):
        return cbpy.trial_comment(0, True)[1]

    def __del__(self):
        cbpy.close()


class vContinuous(rContinuous):
    def __init__(self):
        self.label_ch_dict = {}
        self.n_ch = 0
        self._cmt_buf = []
        self.init()
        self.init_ch_idx()

    def init(self):
        pass

    def init_ch_idx(self):
        print('#init ch idx')
        self.n_ch = 5
        self.label_ch_dict = {'tt5_1':1, 'tt1_1':2, 'tt6_1':9, 'tt2_1':10, 'tt7_1':17}

    def send_comment(self, cmt):
        print('#send: ' + cmt)
        self._cmt_buf.append([time.time(), cmt, 255])

    def get_comment(self):
        res = [i for i in self._cmt_buf]
        self._cmt_buf = []
        return res

    def __del__(self):
        pass


class Logger(QtCore.QObject):
    def __init__(self):
        super(Logger, self).__init__()
        self.log_format = '{time_stamp}, {comment}, {val}' + NEWLINE_CODE
        self.save_path = LOG_SAVE_PATH.rstrip('/')
        self.exe_time = datetime.datetime.now().strftime('%Y-%m-%d-%H%M%S')
        self.save_prefix = ''
        self.recording_start_time = 0
        self.save_filename = None
        self.is_recording = False
        self.fp = None

    def set_save_path(self, save_path):
        self.save_path = save_path

    def set_save_prefix(self, save_prefix):
        self.save_prefix = save_prefix

    def start_recording(self):
        if self.is_recording:
            return -1
        if sys.platform == 'win32':
            try:
                os.system('mkdir ' + '\\'.join([self.save_path, self.exe_time]))
            except:
                pass
        else:
            os.system('mkdir -p ' + '/'.join([self.save_path, self.exe_time]))
        now_datetime =  datetime.datetime.now().strftime('%Y-%m-%d-%H%M%S')
        if sys.platform == 'win32':
            self.save_filename = '\\'.join([self.save_path, self.exe_time, self.save_prefix + now_datetime + '.txt'])
        else:
            self.save_filename = '/'.join([self.save_path, self.exe_time, self.save_prefix + now_datetime + '.txt'])
        self.is_recording = True
        self.recording_start_time = time.time()
        self.fp = open(self.save_filename, 'w')
        self.fp.write('#' + now_datetime + NEWLINE_CODE )
        self.fp.write('#time_stamp, comment, val' + NEWLINE_CODE)

    def stop_recording(self):
        self.is_recording = False
        self.fp.close()

    def set_data(self, comment_list):
        if self.is_recording:
            for com in comment_list:
                self.fp.write(self.log_format.format(time_stamp=com[0], comment=com[1], val=com[2]))




if __name__ == '__main__':
    main()
