#include "ODNN_BThread.h"
#include "../monitor_interval/pcc_mi.h"

ODNN_BThread::ODNN_BThread() :  BThread("ODNN_BThread"), /*log_file_samples("monitor_intervals_bpc.txt", std::ios::out), */
                                simulation_events_file("monitor_intervals_aurora_dl.txt", std::ios::in)
{
    if (!this->python_initialized_) {
        this->InitializePython();
    }

    current_rate = -1.0;
    counter = 1;

    id = 0; // Counter for number of flows (no need at the moment for more than 1).
    has_time_offset = false;
    time_offset_usec = 0;

    const char* python_path_arg = Options::Get("-pypath="); // The location in which the pcc_addon.py file can be found.
    if (python_path_arg != NULL) {
        int python_path_arg_len = strlen(python_path_arg);
        char python_path_cmd_buf[python_path_arg_len + 50];
        sprintf(&python_path_cmd_buf[0], "sys.path.append(\"%s\")", python_path_arg);
        PyRun_SimpleString(&python_path_cmd_buf[0]);
        //std::cerr << "Adding python path: " << python_path_arg << std::endl;
    }

    const char* python_filename = "pcc_rate_controller";
    const char* python_filename_arg = Options::Get("-pyhelper=");
    if (python_filename_arg != NULL) {
        python_filename = python_filename_arg;
    }
    
    module = PyImport_ImportModule(python_filename);
    if (module == NULL) {
        std::cerr << "ERROR: Could not load python module: " << python_filename << std::endl;
        PyErr_Print();
        exit(-1);
    }
    
    PyObject* init_func = PyObject_GetAttrString(module, "init");
    if (init_func == NULL) {
        std::cerr << "ERROR: Could not load python function: init" << std::endl;
        PyErr_Print();
        exit(-1);
    }
    PyObject* id_obj = PyLong_FromLong(id);
    static PyObject* args = PyTuple_New(1);
    PyTuple_SetItem(args, 0, id_obj);
    
    PyObject* init_result = PyObject_CallObject(init_func, args);
    PyErr_Print();
    
    give_sample_func = PyObject_GetAttrString(module, "give_sample");
    if (give_sample_func == NULL) {
        std::cerr << "ERROR: Could not load python function: give_sample" << std::endl;
        PyErr_Print();
        exit(-1);
    }
    
    get_rate_func = PyObject_GetAttrString(module, "get_rate");
    if (get_rate_func == NULL) {
        std::cerr << "ERROR: Could not load python function: get_rate" << std::endl;
        PyErr_Print();
        exit(-1);
    }
    
    reset_func = PyObject_GetAttrString(module, "reset");
    if (reset_func == NULL) {
        std::cerr << "ERROR: Could not load python function: reset" << std::endl;
        PyErr_Print();
        exit(-1);
    }

    counter = 1;
    // We should have 2+10 fields - starts with sending rate
    /*
    log_file_samples << "\ttype\t" << "sending_rate\t" << "bytes_sent\t" << "bytes_acked\t" << "bytes_lost\t";
    log_file_samples << "send_start_time_sec\t" << "send_end_time_sec\t" << "recv_start_time_sec\t" << "recv_end_time_sec\t";
    log_file_samples << "first_ack_latency_sec\t" << "last_ack_latency_sec\t" << "packet_size\t" << "utility\n";
    */
}

ODNN_BThread::~ODNN_BThread()
{
    log_file_samples.close();
    simulation_events_file.close();
}


void ODNN_BThread::entryPoint()
{
    printf("***Enter to ODNN_BThread...***\n" );

    Vector<Event> requested;
    Vector<Event> watched;
    Vector<Event> blocked;

    // Run the simulation log files collected from Aurora DL
    // testModel();
    // return;

    int id = 1;
    while(true)
    {
        Event monitorIntervalEvent(0, id);
        Event queryNextSendingRateEvent(1, id);
        requested.clear();
        watched.clear();
        blocked.clear();
        watched.append(monitorIntervalEvent);
        watched.append(queryNextSendingRateEvent);
        bSync(requested, watched, blocked, "ODNN_BThread");
        Event lastEvent = this->lastEvent();
        
        if (lastEvent.type() == 0) // monitorIntervalEvent
        {
            MonitorIntervalFinished(lastEvent.monitorInterval());
        }

        if (lastEvent.type() == 1) // queryNextSendingRateEvent
        {
            QuicBandwidth result = GetNextSendingRate();
            int event_id = lastEvent.id();
            Event updateSendingRate(2, event_id, NULL, result);
            requested.clear();
            requested.append(updateSendingRate);
            watched.clear();
            blocked.clear();
            blocked.append(monitorIntervalEvent);
            blocked.append(queryNextSendingRateEvent);
            bSync(requested, watched, blocked, "ODNN_BThread"); 
        }

        ++id;
    }

    done();
    printf("Leave ODNN_BThread...\n" );
}

void ODNN_BThread::InitializePython() {
    Py_Initialize();
    PyRun_SimpleString("import sys");

    std::stringstream set_argv_ss;
    set_argv_ss << "sys.argv = [";
    wchar_t** unicode_args = new wchar_t*[Options::argc];
    for (int i = 0; i < Options::argc; ++i) {
        const char* arg = Options::argv[i];
        if (i == 0) {
            set_argv_ss << "\"" << arg << "\"";
        } else {
            set_argv_ss << ", \"" << arg << "\"";
        }
        int len = strlen(arg);
        std::wstring wc(len, L'#' );
        mbstowcs(&wc[0], arg, len);
        unicode_args[i] = &wc[0];
    }
    set_argv_ss << "]";
    std::string set_argv_str = set_argv_ss.str();
    PyRun_SimpleString(set_argv_str.c_str());

    this->python_initialized_ = true;
}

void ODNN_BThread::GiveSample(int bytes_sent,
                                         int bytes_acked,
                                         int bytes_lost,
                                         double send_start_time_sec,
                                         double send_end_time_sec,
                                         double recv_start_time_sec,
                                         double recv_end_time_sec,
                                         double first_ack_latency_sec,
                                         double last_ack_latency_sec,
                                         int packet_size,
                                         double utility,
                                         MonitorInterval* mi) {
    
    float rtt_dev = 0.0;
    if(mi != NULL)
    {
        rtt_dev = UtilityCalculator::ComputeRttDeviation(mi);
    }
    this->statisticsFileHandler->LogGiveSample( bytes_sent, bytes_acked, bytes_lost, send_start_time_sec, 
                                                send_end_time_sec, recv_start_time_sec, recv_end_time_sec, 
                                                first_ack_latency_sec, last_ack_latency_sec, packet_size, 
                                                utility, rtt_dev);

    static PyObject* args = PyTuple_New(11);
    
    // flow_id
    PyTuple_SetItem(args, 0, PyLong_FromLong(id));
    
    // bytes_sent
    PyTuple_SetItem(args, 1, PyLong_FromLong(bytes_sent));
    
    // bytes_acked
    PyTuple_SetItem(args, 2, PyLong_FromLong(bytes_acked));
    
    // bytes_lost
    PyTuple_SetItem(args, 3, PyLong_FromLong(bytes_lost));
    
    // send_start_time
    PyTuple_SetItem(args, 4, PyFloat_FromDouble(send_start_time_sec));
    
    // send_end_time
    PyTuple_SetItem(args, 5, PyFloat_FromDouble(send_end_time_sec));
    
    // recv_start_time
    PyTuple_SetItem(args, 6, PyFloat_FromDouble(recv_start_time_sec));
    
    // recv_end_time
    PyTuple_SetItem(args, 7, PyFloat_FromDouble(recv_end_time_sec));

    // rtt_samples
    PyObject* rtt_samples = PyList_New(2);
    PyList_SetItem(rtt_samples, 0, PyLong_FromLong(first_ack_latency_sec));
    PyList_SetItem(rtt_samples, 1, PyLong_FromLong(last_ack_latency_sec));
    PyTuple_SetItem(args, 8, rtt_samples);
    
    // packet_size
    PyTuple_SetItem(args, 9, PyLong_FromLong(packet_size));
    
    // recv_end_time
    PyTuple_SetItem(args, 10, PyFloat_FromDouble(utility));
    
    PyObject_CallObject(give_sample_func, args);
}

void ODNN_BThread::MonitorIntervalFinished(MonitorInterval* mi) {
    if (!has_time_offset) {
        time_offset_usec = mi->GetSendStartTime();
        has_time_offset = true;
    }

    GiveSample(
        mi->GetBytesSent(),
        mi->GetBytesAcked(),
        mi->GetBytesLost(),
        (mi->GetSendStartTime() - time_offset_usec) / (double)USEC_PER_SEC,
        (mi->GetSendEndTime() - time_offset_usec) / (double)USEC_PER_SEC,
        (mi->GetRecvStartTime() - time_offset_usec) / (double)USEC_PER_SEC,
        (mi->GetRecvEndTime() - time_offset_usec) / (double)USEC_PER_SEC,
        mi->GetFirstAckLatency() / (double)USEC_PER_SEC,
        mi->GetLastAckLatency() / (double)USEC_PER_SEC,
        mi->GetAveragePacketSize(),
        mi->GetUtility(),
        mi
    );
}

QuicBandwidth ODNN_BThread::GetNextSendingRate() {
    
    PyObject* id_obj = PyLong_FromLong(id);
    static PyObject* args = PyTuple_New(1);
    PyTuple_SetItem(args, 0, id_obj);
    
    PyObject* result = PyObject_CallObject(get_rate_func, args);
    if (result == NULL) {
        std::cout << "ERROR: Failed to call python get_rate() func" << std::endl;
        PyErr_Print();
        exit(-1);
    }
    
    double result_double = PyFloat_AsDouble(result);
    PyErr_Print();
    if (!PyFloat_Check(result)) {
        std::cout << "ERROR: Output from python get_rate() is not a float" << std::endl;
        exit(-1);
    }
    Py_DECREF(result);

    this->statisticsFileHandler->LogSendingRate(result_double);
    return result_double;
}

void ODNN_BThread::testModel() {
    
    int counter = 1;
    std::string currLine = "";
    while(getNextLine(currLine))
    {
        if (currLine.find("sending_rate") != std::string::npos)
        {
            continue;
        }

        std::vector<double> vec = parseLine(currLine);
        
        if(isMonitorEvent(vec))
        {
            testMonitorInterval(vec);
        }
        else
        {
            QuicBandwidth result = GetNextSendingRate();
        }
        ++counter;
    }

    return;
}

void ODNN_BThread::testMonitorInterval(std::vector<double> vec) {
    
    if(vec.size() > 12) {
        int bytes_sent = vec[2];
        int bytes_acked = vec[3];
        int bytes_lost = vec[4];
        double send_start_time_sec = vec[5];
        double send_end_time_sec = vec[6];
        double recv_start_time_sec = vec[7];
        double recv_end_time_sec = vec[8];
        double first_ack_latency_sec = vec[9];
        double last_ack_latency_sec = vec[10];
        int packet_size = vec[11];
        double utility = vec[12];
        GiveSample( bytes_sent, bytes_acked, bytes_lost, send_start_time_sec,
                    send_end_time_sec, recv_start_time_sec, recv_end_time_sec,
                    first_ack_latency_sec, last_ack_latency_sec, packet_size,
                    utility);
    }
}

bool ODNN_BThread::getNextLine(std::string& line) {

    if (simulation_events_file.is_open()) 
    {
        if(getline(simulation_events_file, line))
        {
            return true;
        }
    }

    return false;
}

std::vector<double> ODNN_BThread::parseLine(std::string& line) {
    std::stringstream input_stringstream(line);
    std::vector<double> values;
    std::string word;
    
    while (getline(input_stringstream, word, '\t'))
    {
        if(!word.empty())
        {
            double word_double = std::stod(word);
            values.push_back(word_double);
        }
    }

    return values;
}

bool ODNN_BThread::isMonitorEvent(std::vector<double> vec) {

    if(vec.size() > 2)
    {
        if(vec[1] == 1.0) // 1.0 stands for monitor interval event
        {
            return true;
        }
    }
    return false; // 2.0 stands for next sending rate event
}

void ODNN_BThread::setStatisticsFileHandler(StatisticsFileHandler* statisticsFileHandler)
{
    this->statisticsFileHandler = statisticsFileHandler;
}