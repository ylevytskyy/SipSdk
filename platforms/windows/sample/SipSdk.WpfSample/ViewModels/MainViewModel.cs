using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;
using System.Windows.Threading;

namespace SipSdk.WpfSample.ViewModels
{
    public class MainViewModel : INotifyPropertyChanged, IDisposable
    {
        private readonly SipClient _client;
        private SipCall? _currentCall;
        private readonly DispatcherTimer _durationTimer;

        private string _username = "1001";
        private string _domain = "127.0.0.1";
        private string _password = "test1001";
        private string _port = "5060";
        private string _transport = "UDP";
        private string _registrationStatus = "Unregistered";
        private bool _isRegistered = false;

        private string _targetUri = "sip:9001@127.0.0.1:5060";
        private string _callStateText = "Idle";
        private bool _isCallActive = false;
        private bool _isOnHold = false;
        private bool _isMuted = false;
        private string _callDurationText = "00:00";

        public event PropertyChangedEventHandler? PropertyChanged;

        public string Username { get => _username; set { _username = value; OnPropertyChanged(); } }
        public string Domain { get => _domain; set { _domain = value; OnPropertyChanged(); } }
        public string Password { get => _password; set { _password = value; OnPropertyChanged(); } }
        public string Port { get => _port; set { _port = value; OnPropertyChanged(); } }
        public string Transport { get => _transport; set { _transport = value; OnPropertyChanged(); } }
        public string RegistrationStatus { get => _registrationStatus; set { _registrationStatus = value; OnPropertyChanged(); } }
        public bool IsRegistered { get => _isRegistered; set { _isRegistered = value; OnPropertyChanged(); } }

        public string TargetUri { get => _targetUri; set { _targetUri = value; OnPropertyChanged(); } }
        public string CallStateText { get => _callStateText; set { _callStateText = value; OnPropertyChanged(); } }
        public bool IsCallActive { get => _isCallActive; set { _isCallActive = value; OnPropertyChanged(); } }
        public bool IsOnHold { get => _isOnHold; set { _isOnHold = value; OnPropertyChanged(); } }
        public bool IsMuted { get => _isMuted; set { _isMuted = value; OnPropertyChanged(); } }
        public string CallDurationText { get => _callDurationText; set { _callDurationText = value; OnPropertyChanged(); } }

        public ICommand RegisterCommand { get; }
        public ICommand UnregisterCommand { get; }
        public ICommand AudioCallCommand { get; }
        public ICommand VideoCallCommand { get; }
        public ICommand HangupCommand { get; }
        public ICommand ToggleHoldCommand { get; }
        public ICommand ToggleMuteCommand { get; }
        public ICommand SendDtmfCommand { get; }

        public MainViewModel()
        {
            _client = new SipClient();
            _client.Start();

            _client.RegistrationStateChanged += OnRegistrationStateChanged;
            _client.IncomingCall += OnIncomingCall;

            _durationTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _durationTimer.Tick += OnDurationTimerTick;

            RegisterCommand = new RelayCommand(_ => ExecuteRegister());
            UnregisterCommand = new RelayCommand(_ => ExecuteUnregister());
            AudioCallCommand = new RelayCommand(_ => ExecuteMakeCall(false));
            VideoCallCommand = new RelayCommand(_ => ExecuteMakeCall(true));
            HangupCommand = new RelayCommand(_ => ExecuteHangup());
            ToggleHoldCommand = new RelayCommand(_ => ExecuteToggleHold());
            ToggleMuteCommand = new RelayCommand(_ => ExecuteToggleMute());
            SendDtmfCommand = new RelayCommand(digit => ExecuteSendDtmf(digit?.ToString() ?? ""));
        }

        private void ExecuteRegister()
        {
            int port = int.TryParse(Port, out int p) ? p : 5060;
            RegistrationStatus = "Registering...";
            _client.Register(Username, Domain, Password, Transport, port);
        }

        private void ExecuteUnregister()
        {
            _client.Unregister();
            RegistrationStatus = "Unregistered";
            IsRegistered = false;
        }

        private void ExecuteMakeCall(bool withVideo)
        {
            if (string.IsNullOrWhiteSpace(TargetUri)) return;

            try
            {
                _currentCall = _client.MakeCall(TargetUri, withVideo);
                AttachCallEvents(_currentCall);
                IsCallActive = true;
                CallStateText = "Calling...";
                _durationTimer.Start();
            }
            catch (Exception ex)
            {
                CallStateText = $"Call failed: {ex.Message}";
            }
        }

        private void ExecuteHangup()
        {
            if (_currentCall != null)
            {
                _currentCall.Hangup();
                _currentCall.Dispose();
                _currentCall = null;
            }
            IsCallActive = false;
            CallStateText = "Idle";
            _durationTimer.Stop();
            CallDurationText = "00:00";
        }

        private void ExecuteToggleHold()
        {
            if (_currentCall != null)
            {
                bool newHold = !IsOnHold;
                _currentCall.SetHold(newHold);
                IsOnHold = newHold;
            }
        }

        private void ExecuteToggleMute()
        {
            if (_currentCall != null)
            {
                bool newMute = !IsMuted;
                _currentCall.SetMuted(newMute);
                IsMuted = newMute;
            }
        }

        private void ExecuteSendDtmf(string digit)
        {
            if (_currentCall != null && !string.IsNullOrEmpty(digit))
            {
                _currentCall.SendDtmf(digit);
            }
        }

        private void AttachCallEvents(SipCall call)
        {
            call.StateChanged += (s, e) =>
            {
                CallStateText = $"{e.State} ({e.StatusCode} {e.Reason})";
                if (e.State == SipCallState.Closed)
                {
                    IsCallActive = false;
                    _durationTimer.Stop();
                    _currentCall = null;
                }
            };

            call.MediaChanged += (s, e) =>
            {
                IsOnHold = e.IsOnHold;
                IsMuted = e.IsMuted;
            };
        }

        private void OnRegistrationStateChanged(object? sender, RegistrationStateChangedEventArgs e)
        {
            IsRegistered = e.State == SipRegistrationState.Registered;
            RegistrationStatus = $"{e.State} ({e.StatusCode} {e.Reason})";
        }

        private void OnIncomingCall(object? sender, IncomingCallEventArgs e)
        {
            _currentCall = e.Call;
            AttachCallEvents(_currentCall);
            IsCallActive = true;
            CallStateText = $"Incoming call from {e.Call.RemoteUri}";
            _durationTimer.Start();
        }

        private void OnDurationTimerTick(object? sender, EventArgs e)
        {
            if (_currentCall != null)
            {
                uint dur = _currentCall.Duration;
                CallDurationText = $"{dur / 60:D2}:{dur % 60:D2}";
            }
        }

        protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        public void Dispose()
        {
            _durationTimer.Stop();
            _currentCall?.Dispose();
            _client.Dispose();
        }
    }

    public class RelayCommand : ICommand
    {
        private readonly Action<object?> _execute;
        private readonly Predicate<object?>? _canExecute;

        public RelayCommand(Action<object?> execute, Predicate<object?>? canExecute = null)
        {
            _execute = execute ?? throw new ArgumentNullException(nameof(execute));
            _canExecute = canExecute;
        }

        public bool CanExecute(object? parameter) => _canExecute == null || _canExecute(parameter);
        public void Execute(object? parameter) => _execute(parameter);
        public event EventHandler? CanExecuteChanged
        {
            add => CommandManager.RequerySuggested += value;
            remove => CommandManager.RequerySuggested -= value;
        }
    }
}
