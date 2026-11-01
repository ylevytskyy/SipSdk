using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace SipSdk.WpfSample.Controls
{
    /// <summary>
    /// Custom WPF video panel rendering real-time BGRA32 decoded video frames
    /// via high-performance WriteableBitmap.
    /// </summary>
    public class SipVideoPanel : ContentControl
    {
        private readonly Image _image;
        private WriteableBitmap? _bitmap;
        private int _currentWidth;
        private int _currentHeight;

        public SipVideoPanel()
        {
            _image = new Image
            {
                Stretch = Stretch.Uniform,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            };
            Content = _image;
            Background = new SolidColorBrush(Color.FromRgb(20, 24, 33));
        }

        public void UpdateFrame(byte[] bgraPixels, int width, int height)
        {
            if (bgraPixels == null || width <= 0 || height <= 0) return;

            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (_bitmap == null || _currentWidth != width || _currentHeight != height)
                {
                    _currentWidth = width;
                    _currentHeight = height;
                    _bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgra32, null);
                    _image.Source = _bitmap;
                }

                int stride = width * 4;
                _bitmap.WritePixels(new Int32Rect(0, 0, width, height), bgraPixels, stride, 0);
            }));
        }

        public void Clear()
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                _image.Source = null;
                _bitmap = null;
                _currentWidth = 0;
                _currentHeight = 0;
            }));
        }
    }
}
