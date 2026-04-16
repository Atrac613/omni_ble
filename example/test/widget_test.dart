// This is a basic Flutter widget test.
//
// To perform an interaction with a widget in your test, use the WidgetTester
// utility in the flutter_test package. For example, you can send tap and scroll
// gestures. You can also use WidgetTester to find child widgets in the widget
// tree, read text, and verify that the values of widget properties are correct.

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:omni_ble_example/main.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('omni_ble/methods');

  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (methodCall) async {
          if (methodCall.method == 'getCapabilities') {
            return {
              'platform': 'test',
              'platformVersion': '1.0',
              'availableFeatures': <String>[],
            };
          }
          if (methodCall.method == 'checkPermissions') {
            return {
              'permissions': {
                'scan': 'notRequired',
                'connect': 'notRequired',
                'advertise': 'notRequired',
              },
              'allGranted': true,
            };
          }
          return null;
        });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  testWidgets('renders scaffold details', (WidgetTester tester) async {
    await tester.pumpWidget(const OmniBleExampleApp());
    await tester.pumpAndSettle();

    expect(find.text('omni_ble scaffold'), findsOneWidget);
    expect(find.textContaining('test 1.0'), findsOneWidget);
    expect(
      find.text('No platform features are marked as implemented yet.'),
      findsOneWidget,
    );
  });
}
