#include <libmary/libmary.h>

#include <mycpp/mycpp.h>
#include <mycpp/cmdline.h>

#include <cstdlib>


using namespace M;

namespace {

class Options
{
public:
    bool help;

    Uint32 frame_duration;
    Uint32 frame_size;
    Uint32 burst_width;

    bool got_server_addr;
    IpAddress server_addr;

    Uint64 exit_after;

    Options ()
	: help (false),

	  frame_duration (40),
	  frame_size (2500),
	  burst_width (1),

	  got_server_addr (false),

	  exit_after ((Uint64) -1)
    {
    }
};

mt_const Options options;

void printUsage ()
{
    outs->print ("Usage: bmeter_srv [options]\n"
	         "Options:\n"
		 "  -d --duration <number>  Frame duration in milliseconds (default: 40)\n"
		 "  -s --size <number>      Frame size in bytes (default: 2500)\n"
		 "  -b --burst <number>     Burst width (number of frames to send with zero interval (default: 1)\n"
		 "  --bind <address>        ip_address:tcp_port to listen (default: *:7777)\n"
		 "  --exit-after <number>   Exit after specified timeout in seconds.\n"
		 "  -h --help               Show help message.\n");
    outs->flush ();
}

class ClientSessionList_name;

class ClientSession : public Object,
		      public IntrusiveListElement<ClientSessionList_name>
{
public:
    bool valid;

    TcpConnection tcp_conn;
    DeferredConnectionSender deferred_sender;
    DeferredProcessor::Registration deferred_reg;

    mt_mutex (::mutex) PollGroup::PollableKey pollable_key;

    // TEST
    Byte *test_buf;

    ClientSession ()
	: valid (true),
	  tcp_conn (this),
	  deferred_sender (this)
    {
    }
};

typedef IntrusiveList<ClientSession, ClientSessionList_name> ClientSessionList;

mt_mutex (mutex) ClientSessionList session_list;

Mutex mutex;

mt_mutex (tick_mutex) unsigned page_fill_counter = 0;

Mutex tick_mutex;

ServerApp server_app (NULL);
PagePool page_pool (4096 /* page_size */, 4096 /* min_pages */);
TcpServer tcp_server (NULL);

mt_mutex (mutex) void destroySession (ClientSession * const session)
{
    if (!session->valid)
	return;
    session->valid = false;

    server_app.getPollGroup()->removePollable (session->pollable_key);
    session->deferred_reg.release ();

    session_list.remove (session);
    session->unref ();
}

void processError (Exception * const exc_,
		   void      * const _session)
{
    ClientSession * const session = static_cast <ClientSession*> (_session);

    if (exc_)
	logD_  (_func, "exc: ", exc_->toString());
    else
	logD_ (_func_);

    mutex.lock ();
    destroySession (session);
    mutex.unlock ();
}

Connection::InputFrontend const conn_input_frontend = {
    NULL /* processInput */,
    processError
};

void senderClosed (Exception * const exc_,
		   void      * const _session)
{
    ClientSession * const session = static_cast <ClientSession*> (_session);

    if (exc_)
	logD_  (_func, "exc: ", exc_->toString());
    else
	logD_ (_func_);

    mutex.lock ();
    destroySession (session);
    mutex.unlock ();
}

Sender::Frontend const sender_frontend = {
    NULL /* sendStateChanged */,
    senderClosed
};

void frameTimerTick (void * const /* cb_data */)
{
    tick_mutex.lock ();

    for (Uint64 i = 0; i < options.burst_width; ++i) {
	PagePool::PageListHead page_list;
	{
	  // Filling page_list.

	    page_pool.getPages (&page_list, options.frame_size);

	    {
		PagePool::Page *page = page_list.first;
		while (page) {
		    memset (page->getData(), (int) page_fill_counter, page->data_len);
		    page = page->getNextMsgPage();
		}
	    }

	    if (page_fill_counter < 255)
		++page_fill_counter;
	    else
		page_fill_counter = 0;
	}

	ClientSessionList::iter iter (session_list);
	while (!session_list.iter_done (iter)) {
	    ClientSession * const session = session_list.iter_next (iter);

#if 0
	    // TEST
	    {
		Size copied = 0;
		PagePool::Page *page = page_list.first;
		while (page) {
		    memcpy (session->test_buf + copied, page->getData(), page->data_len);
		    copied += page->data_len;
		    page = page->getNextMsgPage();
		}
	    }
#endif

	    Sender::MessageEntry_Pages * const msg_pages =
		    Sender::MessageEntry_Pages::createNew (0 /* max_header_len */);
	    msg_pages->header_len = 0;
	    msg_pages->page_pool = &page_pool;
	    msg_pages->first_page = page_list.first;
	    msg_pages->msg_offset = 0;

	    page_pool.msgRef (page_list.first);
	    session->deferred_sender.sendMessage (msg_pages);
	    session->deferred_sender.flush ();
	}

	page_pool.msgUnref (page_list.first);
    }

    tick_mutex.unlock ();
}

bool acceptOneConnection ()
{
    Ref<ClientSession> const session = grab (new ClientSession);
    // TEST
    session->test_buf = new Byte [options.frame_size];

    {
	TcpServer::AcceptResult const res = tcp_server.accept (&session->tcp_conn);
	if (res == TcpServer::AcceptResult::Error) {
	    logE_ (_func, "tcp_server.accept() failed: ", exc->toString());
	    return false;
	}

	if (res == TcpServer::AcceptResult::NotAccepted)
	    return false;

	assert (res == TcpServer::AcceptResult::Accepted);
    }

    session->tcp_conn.setInputFrontend (
	    Cb<Connection::InputFrontend> (&conn_input_frontend, session, session));

    session->deferred_sender.setConnection (&session->tcp_conn);
    session->deferred_sender.setDeferredRegistration (&session->deferred_reg);
    session->deferred_sender.setFrontend (
	    CbDesc<Sender::Frontend> (&sender_frontend, session, session));

    mutex.lock ();
    session->pollable_key = server_app.getPollGroup()->addPollable (session->tcp_conn.getPollable(),
								    &session->deferred_reg);
    if (!session->pollable_key) {
	mutex.unlock ();
	logE_ (_func, "PollGroup::addPollable() failed: ", exc->toString());
	return true;
    }

    session_list.append (session);
    mutex.unlock ();
    session->ref ();

    return true;
}

void accepted (void * /* cb_data */)
{
    logD_ (_func, "Connection accepted");

    while (acceptOneConnection ());
}

TcpServer::Frontend const tcp_server_frontend = {
    accepted
};

void exitTimerTick (void * const /* cb_data */)
{
    logD_ (_func, "Exit timer expired (", options.exit_after, " seconds)");
    server_app.stop ();
}

Result runServer ()
{
    if (!options.got_server_addr) {
	if (!setIpAddress (":7777", &options.server_addr)) {
	    logE_ (_func, "setIpAddress() failed");
	    return Result::Failure;
	}
    }

    if (!server_app.init ()) {
	logE_ (_func, "server_app.init() failed: ", exc->toString());
	return Result::Failure;
    }

    if (!tcp_server.open ()) {
	logE_ (_func, "tcp_server.open() failed: ", exc->toString());
	return Result::Failure;
    }

    tcp_server.setFrontend (Cb<TcpServer::Frontend> (&tcp_server_frontend, NULL, NULL));

    if (!tcp_server.bind (options.server_addr)) {
	logE_ (_func, "tcp_server.bind() failed: ", exc->toString());
	return Result::Failure;
    }

    if (!tcp_server.listen ()) {
	logE_ (_func, "tcp_server.listen() failed: ", exc->toString());
	return Result::Failure;
    }

    server_app.getPollGroup()->addPollable (tcp_server.getPollable(), NULL /* ret_reg */);

    server_app.getTimers()->addTimer_microseconds (frameTimerTick,
						   NULL /* cb_data */,
						   NULL /* coderef_container */,
						   (Time) (options.frame_duration * 1000 * options.burst_width),
						   true /* periodical */);

    if (options.exit_after != (Uint64) -1) {
	server_app.getTimers()->addTimer (exitTimerTick,
					  NULL /* cb_data */,
					  NULL /* coderef_container */,
					  options.exit_after,
					  false /* periodical */);
    }

    logI_ (_func, "Starting...");
    if (!server_app.run ())
	logE_ (_func, "server_app.run() failed: ", exc->toString());
    logI_ (_func, "...Finished");

    {
	ClientSessionList::iter iter (session_list);
	while (!session_list.iter_done (iter)) {
	    ClientSession * const session = session_list.iter_next (iter);
	    destroySession (session);
	}
    }

    return Result::Success;
}

bool cmdline_help (char const * /* short_name */,
		   char const * /* long_name */,
		   char const * /* value */,
		   void       * /* opt_data */,
		   void       * /* cb_data */)
{
    options.help = true;
    return true;
}

bool cmdline_duration (char const * /* short_name */,
		       char const * /* long_name */,
		       char const *value,
		       void       * /* opt_data */,
		       void       * /* cb_data */)
{
    if (!strToUint32_safe (value, &options.frame_duration)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --duration (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

bool cmdline_size (char const * /* short_name */,
		   char const * /* long_name */,
		   char const *value,
		   void       * /* opt_data */,
		   void       * /* cb_data */)
{
    if (!strToUint32_safe (value, &options.frame_size)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --size (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

bool cmdline_burst (char const * /* short_name */,
		    char const * /* long_name */,
		    char const *value,
		    void       * /* opt_data */,
		    void       * /* cb_data */)
{
    if (!strToUint32_safe (value, &options.burst_width)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --burst (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

bool cmdline_bind (char const * /* short_name */,
		   char const * /* long_name */,
		   char const *value,
		   void       * /* opt_data */,
		   void       * /* cb_data */)
{
    if (!setIpAddress_default (ConstMemory (value, strlen (value)),
			       ConstMemory(),
			       7777 /* default_port */,
			       true /* allow_any_host */,
			       &options.server_addr))
    {
	errs->print ("Invalid value \"", value, "\" "
		     "for --bind (IP:PORT expected)");
	exit (EXIT_FAILURE);
    }
    options.got_server_addr = true;
    return true;
}

bool cmdline_exit_after (char const * /* short_name */,
			 char const * /* long_name */,
			 char const *value,
			 void       * /* opt_data */,
			 void       * /* cb_data */)
{
    if (!strToUint64_safe (value, &options.exit_after)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --exit-after (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

}

int main (int    argc,
	  char **argv)
{
    MyCpp::myCppInit ();
    libMaryInit ();

    {
	unsigned const num_opts = 6;
	MyCpp::CmdlineOption opts [num_opts];

	opts [0].short_name   = "h";
	opts [0].long_name    = "help";
	opts [0].opt_callback = cmdline_help;

	opts [1].short_name = "d";
	opts [1].long_name = "duration";
	opts [1].with_value = true;
	opts [1].opt_callback = cmdline_duration;

	opts [2].short_name = "s";
	opts [2].long_name = "size";
	opts [2].with_value = true;
	opts [2].opt_callback = cmdline_size;

	opts [3].short_name = "b";
	opts [3].long_name = "burst";
	opts [3].with_value = true;
	opts [3].opt_callback = cmdline_burst;

	opts [4].long_name = "bind";
	opts [4].with_value = true;
	opts [4].opt_callback = cmdline_bind;

	opts [5].long_name = "exit-after";
	opts [5].with_value = true;
	opts [5].opt_callback = cmdline_exit_after;

	MyCpp::ArrayIterator<MyCpp::CmdlineOption> opts_iter (opts, num_opts);
	MyCpp::parseCmdline (&argc, &argv, opts_iter, NULL /* callback */, NULL /* callback_data */);
    }

    if (options.help) {
	printUsage ();
	return 0;
    }

    errs->print ("Frame duration: ", options.frame_duration, "\n"
		 "Frame size:     ", options.frame_size, "\n"
		 "Burst width:    ", options.burst_width, "\n");

    if (runServer ())
	return 0;

    return EXIT_FAILURE;
}

