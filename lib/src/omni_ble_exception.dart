class OmniBleException implements Exception {
  const OmniBleException({
    required this.code,
    required this.message,
    this.details,
  });

  final String code;
  final String message;
  final Object? details;

  @override
  String toString() => 'OmniBleException(code: $code, message: $message)';
}
